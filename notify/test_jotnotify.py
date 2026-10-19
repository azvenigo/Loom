# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
"""Tests for jotnotify: the reminder schedule and the checks on what the phone sends.

    python3 -m unittest -v notify/test_jotnotify.py
"""

import base64
import json
import os
import tempfile
import types
import unittest
from unittest import mock

import time

# The schedule is in local time (the 08:00 summary), so pin a zone: the times below are all in the
# small hours there, clear of the summary.
os.environ["TZ"] = "America/Los_Angeles"
time.tzset()

import jotnotify as jn

H, D = jn.H, jn.D


def b64(n, lead=b""):
    return base64.urlsafe_b64encode(lead + b"\x01" * (n - len(lead))).rstrip(b"=").decode()


GOOD_SUB = {"endpoint": "https://web.push.apple.com/QGxyz", "keys": {"p256dh": b64(65, b"\x04"), "auth": b64(16)}}


class Schedule(unittest.TestCase):
    def test_offsets_by_lead(self):
        self.assertEqual([n for n, _ in jn.offsets_for(30 * D, False)], ["week", "day", "hour"])
        self.assertEqual([n for n, _ in jn.offsets_for(7 * D, False)], ["day", "hour"])
        self.assertEqual([n for n, _ in jn.offsets_for(5 * H, False)], ["hour"])
        self.assertEqual(jn.offsets_for(30 * 60, False), [("due", 0)])
        self.assertEqual(jn.offsets_for(30, False), [("due", 0)])
        self.assertEqual([n for n, _ in jn.offsets_for(30 * D, True)], ["week", "day"])

    def test_parse_places(self):
        text = "# notes: 1,2\nHome: 47.61012,-122.34211 150 m\nWork: 47.6,-122.3\nBad: 95,10\nnot a place"
        self.assertEqual(jn.parse_places(text), [{"name": "Home", "lat": 47.61012, "lon": -122.34211, "r": 150.0},
                                                 {"name": "Work", "lat": 47.6, "lon": -122.3, "r": 150.0}])

    def test_parse_due(self):
        self.assertFalse(jn.parse_due(["due:2026-10-20t12:00"])[1])
        self.assertTrue(jn.parse_due(["due:2026-10-20"])[1])
        self.assertIsNone(jn.parse_due(["due:friday"]))
        self.assertIsNone(jn.parse_due(["due:2026-13-40"]))


class Subscription(unittest.TestCase):
    def check(self, sub):
        with tempfile.TemporaryDirectory() as d:
            with open(os.path.join(d, "subscription.json"), "w") as f:
                json.dump(sub, f)
            return jn.load_subscription(d)

    def test_accepts_apple(self):
        self.assertIsNotNone(self.check(GOOD_SUB))

    def test_rejects_anything_that_could_reach_elsewhere(self):
        for ep in ("http://web.push.apple.com/x", "https://10.0.0.5/x", "https://web.push.apple.com.evil.test/x",
                   "https://user@web.push.apple.com/x", "https://web.push.apple.com:8443/x", "file:///etc/passwd"):
            with self.subTest(ep=ep):
                self.assertIsNone(self.check(dict(GOOD_SUB, endpoint=ep)))

    def test_rejects_bad_keys(self):
        self.assertIsNone(self.check(dict(GOOD_SUB, keys={"p256dh": b64(64), "auth": b64(16)})))
        self.assertIsNone(self.check(dict(GOOD_SUB, keys={"p256dh": b64(65, b"\x04"), "auth": "not base64!"})))


class Run(unittest.TestCase):
    """Drive run() with a fake Loom and capture what would have been pushed."""

    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        d = self.dir.name
        self.args = types.SimpleNamespace(notify_dir=d, state_dir=d, loom="x", key=os.path.join(d, "k.pem"),
                                          token_file=os.path.join(d, "tok"), page_url="https://example.test/jot/m",
                                          contact="https://example.test", dry_run=True)
        with open(self.args.token_file, "w") as f:
            f.write("t\n")
        jn.init_key(self.args.key)
        self.sent = []
        self.patches = [mock.patch.object(jn.Pusher, "send", lambda s, title, body, link, badge:
                                          self.sent.append((title, body, link, badge))),
                        mock.patch.object(jn, "fetch_places", lambda url: [])]
        for p in self.patches:
            p.start()

    def tearDown(self):
        for p in self.patches:
            p.stop()
        self.dir.cleanup()

    def tick(self, now, todos, actions=()):
        if actions:
            with open(os.path.join(self.dir.name, "actions.jsonl"), "a") as f:
                for a in actions:
                    f.write(json.dumps(a) + "\n")
        self.sent.clear()
        with mock.patch.object(jn, "fetch_todos", lambda url: todos), mock.patch.object(jn.time, "time", lambda: now):
            jn.run(self.args)
        return [s[0] for s in self.sent]

    def todo(self, created, due, title="call the bank"):
        return {"id": int(created * 1e6), "title": title, "due": due, "all_day": False,
                "due_str": str(due), "priority": "low"}

    def test_week_out_gets_day_and_hour_reminders_once(self):
        t0 = 1_800_000_000 + 3 * H   # a quiet hour, away from the morning summary
        t = self.todo(t0, t0 + 7 * D)
        self.assertEqual(self.tick(t0 + 60, [t]), [])
        self.assertEqual(self.tick(t0 + 6 * D + 30, [t]), ["call the bank"])      # a day before
        self.assertEqual(self.tick(t0 + 6 * D + 90, [t]), [])                     # not twice
        self.assertEqual(self.tick(t0 + 7 * D - H + 10, [t]), ["call the bank"])  # an hour before

    def test_short_notice_reminds_at_due_time(self):
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 10 * 60, "take the bread out")
        self.assertEqual(self.tick(t0 + 60, [t]), [])
        self.assertEqual(self.tick(t0 + 9 * 60, [t]), [])
        self.assertEqual(self.tick(t0 + 10 * 60 + 20, [t]), ["take the bread out"])
        self.assertEqual(self.tick(t0 + 11 * 60, [t]), [])

    def test_due_in_a_minute_still_reminds_when_first_seen_late(self):
        # Written at t0, due 40 s later, but triage only lands it in Loom 3 minutes after that.
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 40, "move the car")
        self.assertEqual(self.tick(t0 + 220, [t]), ["move the car"])
        self.assertEqual(self.tick(t0 + 280, [t]), [])
        # ...but not once it is more than ten minutes stale.
        u = self.todo(t0 + 1, t0 + 41, "stale")
        self.assertEqual(self.tick(t0 + 41 + 700, [u]), [])

    def test_test_action_sends_one_push(self):
        t0 = 1_800_000_000 + 3 * H
        self.assertEqual(self.tick(t0, [], [{"action": "test"}, {"action": "test"}]), ["Test from Loom"])
        self.assertEqual(self.tick(t0 + 60, []), [])

    def test_missed_reminders_are_not_sent_late_in_a_burst(self):
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 30 * D)
        # First seen 10 days before due: the week-before reminder is still ahead, nothing is sent now.
        self.assertEqual(self.tick(t0 + 20 * D, [t]), [])
        self.assertEqual(self.tick(t0 + 23 * D + 5, [t]), ["call the bank"])

    def test_mute_and_snooze(self):
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 7 * D)
        self.tick(t0 + 60, [t])
        self.tick(t0 + 120, [t], [{"id": t["id"], "action": "mute"}])
        self.assertEqual(self.tick(t0 + 6 * D + 30, [t]), [])
        self.tick(t0 + 6 * D + 60, [t], [{"id": t["id"], "action": "unmute"},
                                         {"id": t["id"], "action": "snooze", "until": t0 + 6 * D + 2 * H}])
        self.assertEqual(self.tick(t0 + 6 * D + H, [t]), [])
        self.assertEqual(self.tick(t0 + 6 * D + 2 * H + 10, [t]), ["call the bank"])

    def test_done_marks_loom_and_drops_the_todo(self):
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 7 * D)
        marked = []
        with mock.patch.object(jn, "mark_done", lambda url, tid: marked.append(tid)):
            self.tick(t0 + 60, [t], [{"id": t["id"], "action": "done"}, {"id": 999, "action": "done"}])
        self.assertEqual(marked, [t["id"]])   # never an id that is not an open todo
        up = jn.read_json(os.path.join(self.dir.name, "upcoming.json"), None)
        self.assertEqual(up["todos"], [])

    def test_bad_actions_are_ignored(self):
        t0 = 1_800_000_000 + 3 * H
        t = self.todo(t0, t0 + 7 * D)
        self.tick(t0 + 60, [t], [{"id": 12345, "action": "mute"},                     # not an open todo
                                 {"id": t["id"], "action": "rm -rf"},
                                 {"id": t["id"], "action": "snooze", "until": t0 + 400 * D}])
        state = jn.read_json(os.path.join(self.dir.name, "state.json"), None)
        self.assertEqual(set(state["todos"]), {str(t["id"])})
        self.assertNotIn("snooze_until", state["todos"][str(t["id"])])
        self.assertFalse(state["todos"][str(t["id"])].get("muted"))

    def test_upcoming_has_summaries_only(self):
        t0 = 1_800_000_000 + 3 * H
        self.tick(t0, [self.todo(t0, t0 + 2 * D)])
        up = jn.read_json(os.path.join(self.dir.name, "upcoming.json"), None)
        self.assertEqual(set(up["todos"][0]), {"id", "title", "due", "all_day", "priority", "muted", "snoozed_until"})
        self.assertTrue(up["vapid_key"])


if __name__ == "__main__":
    unittest.main()
