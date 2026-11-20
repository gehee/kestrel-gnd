#include "radio/ar8030_link_detail.hpp"
#include "radio/spectator_protocol.hpp"

// A separate receive-only lifecycle: no normal initialization, association,
// stats thread, control/MSP sockets, keyframe requests or air handshake.
void Ar8030Link::run_spectator() {
    auto status = [&](int value) {
        model_.update_ground([&](gnd::RadioState& s) { s.spectator_status = value; });
    };
    while (!*should_stop && spectator_.mode) {
        bb_watchdog_alive();
        model_.take_commands(*this);
        if (!spectator_.mode) break;
        spectator_dirty_ = false;
        spectator_announced_ms_ = 0;
        if (!native_sdio_ || !arlink_request || !arlink_socket_read_now || !arlink_socket_poll_fd || !gnd::valid(spectator_)) {
            status(3);
            usleep(100000);
            continue; // Fail closed, including malformed saved settings.
        }
        bb_host_t* host_handle = nullptr;
        bb_dev_handle_t* device = nullptr;
        bb_dev_list_t* devices = nullptr;
        int video_socket = -1;
        bool started = false;
        int rc = bb_host_connect(&host_handle, host.c_str(), host_port);
        if (!rc) {
            int count = bb_dev_getlist(host_handle, &devices);
            if (count > 0 && devices) device = bb_dev_open(devices[0]);
            if (devices) bb_dev_freelist(devices);
            if (!device) rc = -1;
        }
        if (!rc) {
            bb_sock_opt_t options{};
            options.tx_buf_size = 0x800; options.rx_buf_size = 0xdc00;
            video_socket = bb_socket_open(device, (bb_slot_e)0, 3, 3, &options);
            if (video_socket < 0) rc = -1;
        }
        auto stop = [&]() { return radio::spectator::stop(arlink_request); };
        auto tuning=spectator_;
        auto start = [&]() { return radio::spectator::start(arlink_request,tuning); };
        auto held = [&]() { return radio::spectator::tx_held(arlink_request); };
        // A prior app may have crashed while the radio was still a pilot.
        // Stop that session before probing or configuring the listener. A
        // freshly loaded radio rejects STOP while already held in PSRAM.
        if (!rc) {
            rc=stop();
            if (rc==-7 && radio::spectator::hold_bit(arlink_request)) rc=0;
        }
        if (!rc && !radio::spectator::available(arlink_request)) rc=-95;
        if (!rc) { rc=start(); started=rc==0; }
        if (!rc && !held()) rc=-1;
        status(rc ? 3 : 1);
        printf("ar8030: spectator %d MHz / %d MHz, stream %02X: %s\n",
               spectator_.frequency_mhz, spectator_.bandwidth_mhz, spectator_.stream_id,
               rc ? "unavailable (no pilot fallback)" : "listening, TX held");
        std::vector<uint8_t> bytes(AR_READ_CHUNK);
        uint64_t last=now_ms(), checked=last, reported=0;
        radio::spectator::FollowState following;
        bool received=false;
        while (!rc && !*should_stop && spectator_.mode && !spectator_dirty_) {
            bb_watchdog_alive();
            model_.take_commands(*this);
            if (!spectator_.mode || spectator_dirty_) break;
            int n=arlink_socket_read_now(video_socket,bytes.data(),uint32_t(bytes.size()));
            if (n<0) { rc=n; break; }
            const uint64_t now=now_ms();
            if (n) {
                last=now; received=true; last_data_ms=now; total_bytes+=n;
                consume(bytes.data(),size_t(n));
            } else {
                struct pollfd p{arlink_socket_poll_fd(video_socket),POLLIN,0};
                if (poll(&p,1,50)>0 && (p.revents&(POLLERR|POLLHUP|POLLNVAL))) {rc=-1;break;}
            }
            if (now-reported>=1000) {
                reported=now;
                if (!radio::spectator::state(arlink_request,following)) {rc=-1;break;}
                const int mhz=int(following.khz/1000), bw=5<<(following.bandwidth-2);
                if (tuning.frequency_mhz!=mhz || tuning.bandwidth_mhz!=bw) {
                    drop_pictures();
                    printf("spectator: followed to %d MHz / %d MHz (hops %u, widths %u)\n",mhz,bw,following.hops,following.widths);
                }
                tuning.frequency_mhz=mhz; tuning.bandwidth_mhz=bw;
                const bool live=received && now-last<1000;
                status(live?2:1);
                gnd::LinkStats s{};
                s.active=live; s.state=live?gnd::kLinkUp:0; s.last_update_ms=now;
                s.rx_freq=tuning.frequency_mhz; s.bw_step=int(following.bandwidth); s.mcs=-1;
                model_.link_stats(s);
                if (!live) model_.video_bandwidth(0);
            }
            if (now-last>1000) {
                rc=stop();
                if (!rc) rc=start();
                last=now_ms(); received=false;
            }
            if (now-checked>=30000) {
                if (!held()) rc=-1;
                else printf("ar8030: spectator receive-only check: TX held, DEV/TX/preencode/ACK counters zero\n");
                checked=now_ms();
            }
        }
        // Stop before releasing sockets. Never run normal DEINIT or AUTO/ACS
        // as part of receive-only teardown. INIT explicitly restores pilot.
        if (started) { const int stopped=stop(); if (stopped) rc=stopped; }
        if (video_socket>=0) bb_socket_close(video_socket);
        if (device) bb_dev_close(device);
        if (host_handle) bb_host_disconnect(host_handle);
        drop_pictures();
        accum_base_+=accum.size(); accum.clear(); pkt_end_pos_=0; xor_pending_=false;
        model_.link_state(0); model_.video_bandwidth(0);
        status(rc?3:0);
        if (rc) {
            // Keep the menu responsive while waiting for supported firmware.
            for (int i=0; i<30 && !*should_stop && spectator_.mode && !spectator_dirty_; ++i) {
                model_.take_commands(*this); usleep(100000);
            }
        }
    }
}
