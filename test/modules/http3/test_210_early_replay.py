import re
import shutil
import time

import pytest

from .env import H3Conf


pytestmark = pytest.mark.usefixtures("env")


def install_replay_conf(env, size=16, window=60, policy=None):
    directives = [
        "H3EarlyData on",
        "H3AddressValidation off",
        f"H3EarlyDataReplayCacheSize {size}",
        f"H3EarlyDataReplayWindow {window}",
    ]
    if policy:
        directives.append(f"H3EarlyDataReplayCacheFull {policy}")
    H3Conf(env).add_h3_vhost(directives=directives).install()
    assert env.apache_restart() == 0


def ticket_from_full_handshake(env):
    client = env.h3_client()
    response = client.get("/h3data-0")
    assert response.status == 200, response
    assert client.received_tickets, "server did not issue a TLS session ticket"
    ticket = client.received_tickets[-1]
    assert ticket.max_early_data_size is not None, "ticket does not advertise 0-RTT"
    return ticket


def ignore_expected_replay_logs(env):
    env.httpd_error_log.ignore_recent(matches=[
        r".*rejected a replayed 0-RTT token.*",
        r".*replay cache is full and configured to reject new early data.*",
    ])


class TestEarlyReplay:

    def test_h3_210_01_aioquic_replay_rejected(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env)
        ticket = ticket_from_full_handshake(env)

        first = env.h3_client(session_ticket=ticket).requests_early(
            [dict(path="/h3data-0?first=1")])[0]
        assert first.status == 200, first

        replay = env.h3_client(session_ticket=ticket).requests_early(
            [dict(path="/h3data-0?replay=1")])[0]
        assert replay.status == 425, replay
        assert replay.header("retry-after") == "0"
        assert env.httpd_error_log.scan_recent(
            re.compile(r".*rejected a replayed 0-RTT token"), timeout=5)
        ignore_expected_replay_logs(env)

    def test_h3_210_02_three_clients_replays_rejected(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env, size=8)

        for client_number in range(1, 4):
            ticket = ticket_from_full_handshake(env)
            response = env.h3_client(session_ticket=ticket).requests_early(
                [dict(path=f"/h3data-0?client={client_number}&first=1")])[0]
            assert response.status == 200, (client_number, response)

            replay = env.h3_client(session_ticket=ticket).requests_early(
                [dict(path=f"/h3data-0?client={client_number}&replay=1")])[0]
            assert replay.status == 425, (client_number, replay)
        ignore_expected_replay_logs(env)

    def test_h3_210_03_new_tokens_and_first_packets_unaffected(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env, size=8)

        for index in range(8):
            ticket = ticket_from_full_handshake(env)
            response = env.h3_client(session_ticket=ticket).requests_early(
                [dict(path=f"/h3data-0?independent={index}")])[0]
            assert response.status == 200, (index, response)

    def test_h3_210_04_window_expiry_allows_new_early_use(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env, size=4, window=1)
        ticket = ticket_from_full_handshake(env)

        first = env.h3_client(session_ticket=ticket).requests_early(
            [dict(path="/h3data-0?window=first")])[0]
        assert first.status == 200, first
        immediate = env.h3_client(session_ticket=ticket).requests_early(
            [dict(path="/h3data-0?window=second")])[0]
        assert immediate.status == 425, immediate
        ignore_expected_replay_logs(env)

        time.sleep(1.1)
        after_window = env.h3_client(session_ticket=ticket).requests_early(
            [dict(path="/h3data-0?window=expired")])[0]
        assert after_window.status == 200, after_window

    def test_h3_210_05_capacity_evicts_oldest_by_default(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env, size=1)

        first_ticket = ticket_from_full_handshake(env)
        first = env.h3_client(session_ticket=first_ticket).requests_early(
            [dict(path="/h3data-0?capacity=first")])[0]
        assert first.status == 200, first

        second_ticket = ticket_from_full_handshake(env)
        second = env.h3_client(session_ticket=second_ticket).requests_early(
            [dict(path="/h3data-0?capacity=second")])[0]
        assert second.status == 200, second

        # Capacity one evicted the first token. Its replay is therefore a cache
        # miss and is accepted again; the bounded table never grows.
        first_replay = env.h3_client(session_ticket=first_ticket).requests_early(
            [dict(path="/h3data-0?capacity=first-replay")])[0]
        assert first_replay.status == 200, first_replay

    def test_h3_210_06_capacity_reject_policy_fails_closed(self, env):
        env.require(not env.h3_ossl_backend,
                    "the OpenSSL QUIC backend drops 0-RTT packets")
        install_replay_conf(env, size=1, policy="reject-early")

        first_ticket = ticket_from_full_handshake(env)
        first = env.h3_client(session_ticket=first_ticket).requests_early(
            [dict(path="/h3data-0?full=first")])[0]
        assert first.status == 200, first

        second_ticket = ticket_from_full_handshake(env)
        second = env.h3_client(session_ticket=second_ticket).requests_early(
            [dict(path="/h3data-0?full=second")])[0]
        assert second.status == 503, second
        ignore_expected_replay_logs(env)

    def test_h3_210_07_curl_replay_rejected(self, env, tmp_path):
        env.require_curl_http3()
        version = env.run([env.curl, "--version"]).stdout
        env.require("SSLS-EXPORT" in version and "--tls-earlydata" in
                    env.run([env.curl, "--help", "all"]).stdout,
                    "curl lacks SSL session import/export or --tls-earlydata")
        install_replay_conf(env)

        ticket_file = tmp_path / "curl-ticket.txt"
        url = f"https://{env.h3_domain}:{env.h3_port}/h3data-0"
        boot = env.h3_get(url + "?curl=boot",
                          options=["--noproxy", "*", "--ssl-sessions", str(ticket_file)])
        if boot.exit_code != 0 or not ticket_file.exists() \
                or not ticket_file.read_text().strip():
            pytest.skip("this curl build does not export an HTTP/3 TLS ticket")
        assert boot.response["status"] == 200, boot

        first_ticket = tmp_path / "first-ticket.txt"
        replay_ticket = tmp_path / "replay-ticket.txt"
        shutil.copyfile(ticket_file, first_ticket)
        shutil.copyfile(ticket_file, replay_ticket)

        first = env.h3_get(url + "?curl=first", options=[
            "--noproxy", "*", "--tls-earlydata",
            "--ssl-sessions", str(first_ticket)])
        assert first.response["status"] == 200, first

        replay = env.h3_get(url + "?curl=replay", options=[
            "--noproxy", "*", "--tls-earlydata",
            "--ssl-sessions", str(replay_ticket)])
        assert replay.response["status"] == 425, replay
        ignore_expected_replay_logs(env)
