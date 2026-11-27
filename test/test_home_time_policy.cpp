// The home-server time policy is an inline function in
// net/homeserver_client.h, so no extra source file has to be linked; the
// runner falls back to its default LINK globs (src/core/*.cpp).
#include "net/homeserver_client.h"

#include <cstdint>

#include "test_harness.h"

namespace {

using thermostat::net::HomeTimePolicyInput;
using thermostat::net::kHomeTimeNtpGraceMs;
using thermostat::net::kHomeTimeRefreshMs;
using thermostat::net::kHomeTimeRetryMs;
using thermostat::net::should_fetch_home_time;

HomeTimePolicyInput make_input(uint64_t now_ms) {
  HomeTimePolicyInput in;
  in.now_ms = now_ms;
  return in;
}

}  // namespace

TEST_CASE(policy_never_overrides_synced_ntp) {
  HomeTimePolicyInput in = make_input(kHomeTimeNtpGraceMs * 10);
  in.ntp_synced = true;
  // Even a long-overdue first attempt is refused once SNTP has completed.
  EXPECT_FALSE(should_fetch_home_time(in));

  in.has_attempted = true;
  in.last_attempt_ms = 0;
  in.has_home_time = true;
  EXPECT_FALSE(should_fetch_home_time(in));

  in.has_home_time = false;
  EXPECT_FALSE(should_fetch_home_time(in));
}

TEST_CASE(policy_waits_for_ntp_grace_before_first_attempt) {
  HomeTimePolicyInput in = make_input(5000);
  in.ntp_started_ms = 5000;

  in.now_ms = 5000 + kHomeTimeNtpGraceMs - 1;
  EXPECT_FALSE(should_fetch_home_time(in));

  // The boundary itself already allows the attempt.
  in.now_ms = 5000 + kHomeTimeNtpGraceMs;
  EXPECT_TRUE(should_fetch_home_time(in));
}

TEST_CASE(policy_retries_on_the_documented_gap_before_success) {
  HomeTimePolicyInput in = make_input(100000);
  in.has_attempted = true;
  in.last_attempt_ms = 100000;

  in.now_ms = 100000 + kHomeTimeRetryMs - 1;
  EXPECT_FALSE(should_fetch_home_time(in));

  in.now_ms = 100000 + kHomeTimeRetryMs;
  EXPECT_TRUE(should_fetch_home_time(in));

  // A first success switches to the refresh cadence below.
  in.has_home_time = true;
  EXPECT_FALSE(should_fetch_home_time(in));
}

TEST_CASE(policy_refreshes_every_six_hours_after_success) {
  HomeTimePolicyInput in = make_input(100000);
  in.has_attempted = true;
  in.has_home_time = true;
  in.last_attempt_ms = 100000;

  in.now_ms = 100000 + kHomeTimeRefreshMs - 1;
  EXPECT_FALSE(should_fetch_home_time(in));

  in.now_ms = 100000 + kHomeTimeRefreshMs;
  EXPECT_TRUE(should_fetch_home_time(in));

  // The refresh gap is much longer than the pre-success retry gap.
  EXPECT_TRUE(kHomeTimeRefreshMs > kHomeTimeRetryMs);
}

TEST_CASE(policy_refuses_when_the_clock_moved_backwards) {
  // now_ms < ntp_started_ms: the grace window is treated as not yet elapsed
  // instead of unsigned-underflowing into an immediate fetch.
  HomeTimePolicyInput in = make_input(5000);
  in.ntp_started_ms = 10000;
  EXPECT_FALSE(should_fetch_home_time(in));

  // now_ms < last_attempt_ms: the retry gap is treated as not yet elapsed.
  in = make_input(5000);
  in.ntp_started_ms = 0;
  in.has_attempted = true;
  in.last_attempt_ms = 10000;
  EXPECT_FALSE(should_fetch_home_time(in));
}

TEST_CASE(policy_attempts_without_any_ntp_start) {
  // Defensive: with no configTime() timestamp a board that has been up past
  // the grace window may still ask the home server.
  HomeTimePolicyInput in = make_input(kHomeTimeNtpGraceMs);
  EXPECT_TRUE(should_fetch_home_time(in));
}

THERMOSTAT_TEST_MAIN()
