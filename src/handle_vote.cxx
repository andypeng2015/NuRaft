/************************************************************************
Modifications Copyright 2017-2019 eBay Inc.
Author/Developer(s): Jung-Sang Ahn

Original Copyright:
See URL: https://github.com/datatechnology/cornerstone

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    https://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
**************************************************************************/

#include "error_code.hxx"
#include "raft_server.hxx"

#include "cluster_config.hxx"
#include "event_awaiter.hxx"
#include "handle_custom_notification.hxx"
#include "peer.hxx"
#include "state_mgr.hxx"
#include "tracer.hxx"

#include <cassert>
#include <sstream>

namespace nuraft {

bool raft_server::check_cond_for_zp_election() {
    ptr<raft_params> params = ctx_->get_params();
    if ( params->allow_temporary_zero_priority_leader_ &&
         target_priority_ == 1 &&
         my_priority_ == 0 &&
         priority_change_timer_.get_ms() >
            (uint64_t)params->heart_beat_interval_ * 20 ) {
        return true;
    }
    return false;
}

void raft_server::request_prevote() {
    ptr<raft_params> params = ctx_->get_params();
    ptr<cluster_config> c_config = get_config();
    for (peer_itor it = peers_.begin(); it != peers_.end(); ++it) {
        ptr<peer> pp = it->second;
        if (!is_regular_member(pp)) continue;
        ptr<srv_config> s_config = c_config->get_server( pp->get_id() );

        if (s_config) {
            bool recreate = false;
            if (hb_alive_) {
                // First pre-vote request: reset RPC client for all peers.
                recreate = true;

            } else {
                // Since second time: reset only if `rpc_` is null.
                recreate = pp->need_to_reconnect();

                // Or if it is not active long time, reconnect as well.
                int32 last_active_time_ms = pp->get_active_timer_us() / 1000;
                if ( last_active_time_ms >
                         params->heart_beat_interval_ *
                             raft_server::raft_limits_.reconnect_limit_ ) {
                    p_wn( "connection to peer %d is not active long time: %d ms, "
                          "need reconnection for prevote",
                          pp->get_id(),
                          last_active_time_ms );
                    recreate = true;
                }
            }

            if (recreate) {
                p_in("reset RPC client for peer %d", s_config->get_id());
                pp->recreate_rpc(s_config, *ctx_);
            }
        }
    }

    int quorum_size = get_quorum_for_election();
    if (pre_vote_.live_ + pre_vote_.dead_ > 0) {
        if (pre_vote_.live_ + pre_vote_.dead_ < quorum_size + 1) {
            // Pre-vote failed due to non-responding voters.
            pre_vote_.no_response_failure_count_++;
            p_wn("total %d nodes (including this node) responded for pre-vote "
                 "(term %" PRIu64 ", live %d, dead %d), at least %d nodes should "
                 "respond. failure count %d",
                 pre_vote_.live_.load() + pre_vote_.dead_.load(),
                 pre_vote_.term_,
                 pre_vote_.live_.load(),
                 pre_vote_.dead_.load(),
                 quorum_size + 1,
                 pre_vote_.no_response_failure_count_.load());
        } else {
            pre_vote_.no_response_failure_count_ = 0;
        }
    }
    int num_voting_members = get_num_voting_members();
    if ( params->auto_adjust_quorum_for_small_cluster_ &&
         num_voting_members == 2 &&
         pre_vote_.no_response_failure_count_ > raft_server::raft_limits_.vote_limit_ ) {
        // 2-node cluster's pre-vote failed due to offline node.
        p_wn("2-node cluster's pre-vote is failing long time, "
             "adjust quorum to 1");

        cb_func::Param cb_param(id_, leader_, -1);
        CbReturnCode rc =
            ctx_->cb_func_.call(cb_func::AutoAdjustQuorum, &cb_param);
        if (rc == CbReturnCode::ReturnNull) {
            // Callback function rejected the adjustment.
            p_wn("quorum size adjustment was declined by callback");
        } else {
            ptr<raft_params> clone = cs_new<raft_params>(*params);
            clone->custom_commit_quorum_size_ = 1;
            clone->custom_election_quorum_size_ = 1;
            ctx_->set_params(clone);
        }
    }

    hb_alive_ = false;
    leader_ = -1;
    role_ = srv_role::candidate;
    index_at_becoming_leader_ = 0;
    pre_vote_.reset(state_->get_term());
    // Count for myself.
    pre_vote_.dead_++;

    if ( my_priority_ < target_priority_ ) {
        if ( check_cond_for_zp_election() ) {
            p_in("[PRIORITY] temporarily allow election for zero-priority member");
        } else {
            p_in("[PRIORITY] will not initiate pre-vote due to priority: "
                 "target %d, mine %d", target_priority_, my_priority_);
            restart_election_timer();
            return;
        }
    }

    p_in("[PRE-VOTE INIT] my id %d, my role %s, term %" PRIu64 ", log idx %" PRIu64 ", "
         "log term %" PRIu64 ", priority (target %d / mine %d)\n",
         id_, srv_role_to_string(role_).c_str(),
         state_->get_term(), log_store_->next_slot() - 1,
         term_for_log(log_store_->next_slot() - 1),
         target_priority_, my_priority_);

    for (peer_itor it = peers_.begin(); it != peers_.end(); ++it) {
        ptr<peer> pp = it->second;
        if (!is_regular_member(pp)) {
            // Do not send voting request to learner.
            continue;
        }

        ptr<req_msg> req( cs_new<req_msg>
                          ( state_->get_term(),
                            msg_type::pre_vote_request,
                            id_,
                            pp->get_id(),
                            term_for_log(log_store_->next_slot() - 1),
                            log_store_->next_slot() - 1,
                            quick_commit_index_.load() ) );
        if (pp->make_busy()) {
            pp->send_req(pp, req, resp_handler_);
        } else {
            pre_vote_.connection_busy_++;
            p_wn("failed to send prevote request: peer %d (%s) is busy, count %d",
                 pp->get_id(), pp->get_endpoint().c_str(),
                 pre_vote_.connection_busy_.load());
        }
    }

    int32 election_quorum_size = get_quorum_for_election() + 1;
    if (pre_vote_.connection_busy_ + election_quorum_size > num_voting_members) {
        // Couldn't send pre-vote request to majority of peers,
        // no hope to get quorum.
        pre_vote_.busy_connection_failure_count_++;
        p_wn("too many busy connections: %d, num voting members: %d, quorum size: %d, "
             "no hope to get quorum, count: %d",
             pre_vote_.connection_busy_.load(),
             num_voting_members,
             election_quorum_size,
             pre_vote_.busy_connection_failure_count_.load());
        int32_t busy_conn_limit = raft_server::raft_limits_.busy_connection_limit_;
        if (busy_conn_limit &&
            pre_vote_.busy_connection_failure_count_ > busy_conn_limit) {
            // LCOV_EXCL_START
            p_ft("too many pre-vote failures due to busy connection!");
            ctx_->state_mgr_->system_exit(N22_unrecoverable_isolation);
            // LCOV_EXCL_STOP
        }
    }
}

void raft_server::initiate_vote(bool force_vote) {
    int grace_period = ctx_->get_params()->grace_period_of_lagging_state_machine_;
    ulong cur_term = state_->get_term();
    if ( !force_vote &&
         grace_period &&
         sm_commit_index_ < lagging_sm_target_index_ ) {
        p_in("grace period option is enabled, and state machine needs catch-up: "
             "%" PRIu64 " vs. %" PRIu64 "",
             sm_commit_index_.load(),
             lagging_sm_target_index_.load());
        if (vote_init_timer_term_ != cur_term) {
            p_in("grace period: %d, term increment detected %" PRIu64 " vs. %" PRIu64
                 ", reset timer",
                grace_period, vote_init_timer_term_.load(), cur_term);
            vote_init_timer_.set_duration_ms(grace_period);
            vote_init_timer_.reset();
            vote_init_timer_term_ = cur_term;
        }

        if ( vote_init_timer_term_ == cur_term &&
             !vote_init_timer_.timeout() ) {
            // Grace period, do not initiate vote.
            p_in("grace period: %d, term %" PRIu64 ", waited %" PRIu64
                 " ms, skip initiating vote",
                 grace_period, cur_term, vote_init_timer_.get_ms());
            return;

        } else {
            p_in("grace period: %d, no new leader detected for term %" PRIu64
                 " for %" PRIu64 " ms",
                 grace_period, cur_term, vote_init_timer_.get_ms());
        }
    }

    if ( my_priority_ >= target_priority_ ||
         force_vote ||
         check_cond_for_zp_election() ||
         ( get_quorum_for_election() == 0 &&
           my_priority_ > 0 ) ) {
        // Request vote when
        //  1) my priority satisfies the target, OR
        //  2) I'm the only node in the group.
        state_->inc_term();
        state_->set_voted_for(-1);
        role_ = srv_role::candidate;
        index_at_becoming_leader_ = 0;
        votes_granted_ = 0;
        votes_responded_ = 0;
        election_completed_ = false;
        // NOTE: Following `request_vote` will call `save_state()`,
        //       hence we don't call it here even though `state_` changes.
        request_vote(force_vote);
    }

    if (role_ != srv_role::leader) {
        hb_alive_ = false;
        leader_ = -1;
    }
}

void raft_server::request_vote(bool force_vote) {
    state_->set_voted_for(id_);
    ctx_->state_mgr_->save_state(*state_);
    votes_granted_ += 1;
    votes_responded_ += 1;
    p_in("[VOTE INIT] my id %d, my role %s, term %" PRIu64 ", log idx %" PRIu64 ", "
         "log term %" PRIu64 ", priority (target %d / mine %d)\n",
         id_, srv_role_to_string(role_).c_str(),
         state_->get_term(), log_store_->next_slot() - 1,
         term_for_log(log_store_->next_slot() - 1),
         target_priority_, my_priority_);

    // is this the only server?
    if (votes_granted_ > get_quorum_for_election()) {
        election_completed_ = true;
        become_leader();
        return;
    }

    for (peer_itor it = peers_.begin(); it != peers_.end(); ++it) {
        ptr<peer> pp = it->second;
        if (!is_regular_member(pp)) {
            // Do not send voting request to learner or new joiner.
            continue;
        }
        ptr<req_msg> req = cs_new<req_msg>
                           ( state_->get_term(),
                             msg_type::request_vote_request,
                             id_,
                             pp->get_id(),
                             term_for_log(log_store_->next_slot() - 1),
                             log_store_->next_slot() - 1,
                             quick_commit_index_.load() );
        if (force_vote) {
            // Add a special log entry to let receivers ignore the priority.

            // Force vote message, and wrap it using log_entry.
            ptr<force_vote_msg> fv_msg = cs_new<force_vote_msg>();
            ptr<log_entry> fv_msg_le =
                cs_new<log_entry>(0, fv_msg->serialize(), log_val_type::custom);

            // Ship it.
            req->log_entries().push_back(fv_msg_le);
        }
        p_db( "send %s to server %d with term %" PRIu64 "",
              msg_type_to_string(req->get_type()).c_str(),
              it->second->get_id(),
              state_->get_term() );
        if (pp->make_busy()) {
            pp->send_req(pp, req, resp_handler_);
        } else {
            p_wn("failed to send vote request: peer %d (%s) is busy",
                 pp->get_id(), pp->get_endpoint().c_str());
        }
    }
}

ptr<resp_msg> raft_server::handle_vote_req(req_msg& req) {
    p_in("[VOTE REQ] my role %s, from peer %d, "
         "log term: req %" PRIu64 " / mine %" PRIu64 "\n"
         "last idx: req %" PRIu64 " / mine %" PRIu64
         ", term: req %" PRIu64 " / mine %" PRIu64 "\n"
         "priority: target %d / mine %d, voted_for %d",
         srv_role_to_string(role_).c_str(),
         req.get_src(), req.get_last_log_term(), log_store_->last_entry()->get_term(),
         req.get_last_log_idx(), log_store_->next_slot()-1,
         req.get_term(), state_->get_term(),
         target_priority_, my_priority_, state_->get_voted_for());

    ptr<resp_msg> resp( cs_new<resp_msg>
                        ( state_->get_term(),
                          msg_type::request_vote_response,
                          id_,
                          req.get_src() ) );

    bool log_okay =
        req.get_last_log_term() > log_store_->last_entry()->get_term() ||
        ( req.get_last_log_term() == log_store_->last_entry()->get_term() &&
          log_store_->next_slot() - 1 <= req.get_last_log_idx() );

    bool grant =
        req.get_term() == state_->get_term() &&
        log_okay &&
        ( state_->get_voted_for() == req.get_src() ||
          state_->get_voted_for() == -1 );

    bool ignore_priority = false;
    if (req.log_entries().size() > 0) {
        p_in("[VOTE REQ] force vote request, will ignore priority");
        ignore_priority = true;
    }
    if (state_->is_catching_up()) {
        p_in("[VOTE REQ] this server is catching-up with leader, "
             "will ignore priority");
        ignore_priority = true;
    }

    if (grant) {
        ptr<cluster_config> c_conf = get_config();
        for (auto& entry: c_conf->get_servers()) {
            srv_config* s_conf = entry.get();
            if ( !ignore_priority &&
                 s_conf->get_id() == req.get_src() &&
                 s_conf->get_priority() &&
                 s_conf->get_priority() < target_priority_ ) {
                // NOTE:
                //   If zero-priority member initiates leader election,
                //   that is intentionally triggered by the flag in
                //   `raft_params`. In such case, we don't check the
                //   priority.
                p_in("I (%d) could vote for peer %d, "
                     "but priority %d is lower than %d",
                     id_, s_conf->get_id(),
                     s_conf->get_priority(), target_priority_);
                p_in("decision: X (deny)\n");
                return resp;
            }
        }

        p_in("decision: O (grant), voted_for %d, term %" PRIu64,
             req.get_src(), resp->get_term());
        resp->accept(log_store_->next_slot());
        state_->set_voted_for(req.get_src());
        ctx_->state_mgr_->save_state(*state_);
    } else {
        p_in("decision: X (deny), term %" PRIu64, resp->get_term());
    }

    return resp;
}

void raft_server::handle_vote_resp(resp_msg& resp) {
    if (election_completed_) {
        p_in("Election completed, will ignore the voting result from this server");
        return;
    }

    if (resp.get_term() != state_->get_term()) {
        // Vote response for other term. Should ignore it.
        p_in("[VOTE RESP] from peer %d, my role %s, "
             "but different resp term %" PRIu64 ". ignore it.",
             resp.get_src(), srv_role_to_string(role_).c_str(), resp.get_term());
        return;
    }
    votes_responded_ += 1;

    if (resp.get_accepted()) {
        votes_granted_ += 1;
    }

    if (votes_responded_ >= get_num_voting_members()) {
        election_completed_ = true;
    }

    int32 election_quorum_size = get_quorum_for_election() + 1;

    p_in("[VOTE RESP] peer %d (%s), resp term %" PRIu64 ", my role %s, "
         "granted %d, responded %d, "
         "num voting members %d, quorum %d\n",
         resp.get_src(), (resp.get_accepted()) ? "O" : "X", resp.get_term(),
         srv_role_to_string(role_).c_str(),
         (int)votes_granted_, (int)votes_responded_,
         get_num_voting_members(), election_quorum_size);

    if (votes_granted_ >= election_quorum_size) {
        p_in("Server is elected as leader for term %" PRIu64, state_->get_term());
        election_completed_ = true;
        become_leader();
        p_in("  === LEADER (term %" PRIu64 ") ===\n", state_->get_term());
    }
}

ptr<resp_msg> raft_server::handle_prevote_req(req_msg& req) {
    ulong next_idx_for_resp = 0;
    auto entry = peers_.find(req.get_src());
    if (entry == peers_.end()) {
        // This node already has been removed, set a special value.
        next_idx_for_resp = std::numeric_limits<ulong>::max();
    }

    p_in("[PRE-VOTE REQ] my role %s, from peer %d, "
         "log term: req %" PRIu64 " / mine %" PRIu64 "\n"
         "last idx: req %" PRIu64 " / mine %" PRIu64
         ", term: req %" PRIu64 " / mine %" PRIu64 "\n"
         "%s",
         srv_role_to_string(role_).c_str(),
         req.get_src(), req.get_last_log_term(),
         log_store_->last_entry()->get_term(),
         req.get_last_log_idx(), log_store_->next_slot()-1,
         req.get_term(), state_->get_term(),
         (hb_alive_) ? "HB alive" : "HB dead");

    ptr<resp_msg> resp
        ( cs_new<resp_msg>
          ( req.get_term(),
            msg_type::pre_vote_response,
            id_,
            req.get_src(),
            next_idx_for_resp ) );

    // NOTE:
    //   While `catching_up_` flag is on, this server does not get
    //   normal append_entries request so that `hb_alive_` may not
    //   be cleared properly. Hence, it should accept any pre-vote
    //   requests.
    if (state_->is_catching_up()) {
        p_in("this server is catching up, always accept pre-vote");
    }
    if (!hb_alive_ || state_->is_catching_up()) {
        p_in("pre-vote decision: O (grant)");
        resp->accept(log_store_->next_slot());
    } else {
        if (next_idx_for_resp != std::numeric_limits<ulong>::max()) {
            p_in("pre-vote decision: X (deny)");
        } else {
            p_in("pre-vote decision: XX (strong deny, non-existing node)");
        }
    }

    return resp;
}

void raft_server::handle_prevote_resp(resp_msg& resp) {
    if (resp.get_term() != pre_vote_.term_) {
        // Vote response for other term. Should ignore it.
        p_in("[PRE-VOTE RESP] from peer %d, my role %s, "
             "but different resp term %" PRIu64 " (pre-vote term %" PRIu64 "). "
             "ignore it.",
             resp.get_src(), srv_role_to_string(role_).c_str(),
             resp.get_term(), pre_vote_.term_);
        return;
    }

    if (resp.get_accepted()) {
        // Accept: means that this peer is not receiving HB.
        pre_vote_.dead_++;
    } else {
        if (resp.get_next_idx() != std::numeric_limits<ulong>::max()) {
            // Deny: means that this peer still sees leader.
            pre_vote_.live_++;
        } else {
            // `next_idx_for_resp == MAX`, it is a special signal
            // indicating that this node has been already removed.
            pre_vote_.abandoned_++;
        }
    }

    int32 election_quorum_size = get_quorum_for_election() + 1;

    p_in("[PRE-VOTE RESP] peer %d (%s), term %" PRIu64 ", resp term %" PRIu64 ", "
         "my role %s, dead %d, live %d, abandoned %d, "
         "num voting members %d, quorum %d\n",
         resp.get_src(), (resp.get_accepted())?"O":"X",
         pre_vote_.term_, resp.get_term(),
         srv_role_to_string(role_).c_str(),
         pre_vote_.dead_.load(), pre_vote_.live_.load(), pre_vote_.abandoned_.load(),
         get_num_voting_members(), election_quorum_size);

    if (pre_vote_.dead_ >= election_quorum_size) {
        p_in("[PRE-VOTE DONE] SUCCESS, term %" PRIu64, pre_vote_.term_);

        bool exp = false;
        bool val = true;
        if (pre_vote_.done_.compare_exchange_strong(exp, val)) {
            p_in("[PRE-VOTE DONE] initiate actual vote");

            // Immediately initiate actual vote.
            initiate_vote();

            // restart the election timer if this is not yet a leader
            if (role_ != srv_role::leader) {
                restart_election_timer();
            }

        } else {
            p_in("[PRE-VOTE DONE] actual vote is already initiated, do nothing");
        }
    }

    if (pre_vote_.live_ >= election_quorum_size) {
        pre_vote_.quorum_reject_count_.fetch_add(1);
        p_wn("[PRE-VOTE] rejected by quorum, count %d",
             pre_vote_.quorum_reject_count_.load());
        if ( pre_vote_.quorum_reject_count_ >=
                 raft_server::raft_limits_.pre_vote_rejection_limit_ ) {
            p_ft("too many pre-vote rejections, probably this node is not "
                 "receiving heartbeat from leader. "
                 "we should re-establish the network connection");
            send_reconnect_request();
        }
    }

    if (pre_vote_.abandoned_ >= election_quorum_size) {
        p_er("[PRE-VOTE DONE] this node has been removed, stepping down");
        cb_func::Param param(id_, leader_);
        CbReturnCode rc = ctx_->cb_func_.call( cb_func::RemovedFromCluster,
                                               &param );
        (void)rc;
        steps_to_down_ = 2;
    }
}

} // namespace nuraft;

