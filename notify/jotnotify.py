#!/usr/bin/env python3
# Copyright (c) 2026 Alexander Zvenigorodsky. MIT License. See LICENSE.
"""jotnotify - phone reminders for Loom todos, through the jot capture page (jot/CapturePage.h).

Runs once a minute (packaging/systemd/jotnotify.timer). Each run:
  1. reads open todos with a due date from Loom's REST API
  2. applies the snooze / mute requests the phone posted (jot appends them to actions.jsonl)
  3. sends every reminder that has come due, and the morning "due today" summary
  4. writes upcoming.json, which jot serves to the page at GET /jot/upcoming - the todos, and the
     named places from Loom's known-places jot so the page can say "at Home" (persist/Places.h)

WHO DOES WHAT. jot is the internet-facing half and only moves bytes between the phone and three
files in the notify directory; it never parses them and never talks to Loom. Everything those
bytes mean, and every check on them, is here. Treat all three as written by whoever holds the
token. In particular a push subscription names the URL this process will POST to, so its endpoint
is held to Apple's push service: without that, a forged subscription would turn this process into a
way to make the host send requests anywhere on the LAN.

REMINDER SCHEDULE, from the lead time (due minus when the todo was written - a jot's id is its
creation time):
    lead >= 14 days   a week before, a day before, an hour before
    lead >=  2 days   a day before, an hour before
    lead >   1 hour   an hour before
    shorter           at the due time (sent up to 10 minutes late, since a new jot takes a moment
                      to reach Loom)
plus one 08:00 summary of what is due today. A date-only due ("due:2026-10-20") counts as 09:00
for the week/day reminders and gets no hour-before one; the summary covers it on the day.

Reminders whose time had already passed when a todo (or its new due date) was first seen are
skipped, not sent late - a backlog would otherwise arrive as a burst. If a run finds several due at
once it sends only the nearest one.

Done adds status:done to the todo in Loom (keeping todo, Loom's convention) - the only write this
process makes there, so jot itself still never touches Loom.

Snooze holds every reminder for that todo until the snooze ends, then sends one. It never moves the
due date; that is the dashboard's snooze, and the two are deliberately different things. Mute stops
reminders for that todo until unmuted.

Uses Declarative Web Push (iOS 18.4+): the payload is the notification itself, so the page needs no
service worker. See the ios-web-app-capabilities-2026 jot in Loom.
"""

import argparse
import base64
import datetime as dt
import json
import os
import re
import sys
import tempfile
import time
import urllib.parse
import urllib.request

H = 3600
D = 86400

# The only push services a subscription may point at. Apple's is the one this page can produce
# (Safari home-screen web apps). Widening this list widens what a token holder can make this host
# connect to - keep it to real push services.
ALLOWED_PUSH_HOSTS = {"web.push.apple.com"}

DIGEST_HOUR = 8          # the morning summary
DIGEST_LATEST_HOUR = 20  # after a long outage, a "due today" summary at 11pm is noise
MAX_ACTIONS = 200        # per run; a flood of actions cannot make a run take forever
UPCOMING_DAYS = 60       # how far ahead the page's list looks
OVERDUE_DAYS = 30        # and how far back

DUE_RE = re.compile(r"^due:(\d{4})-(\d{2})-(\d{2})(?:t(\d{2}):(\d{2}))?$")
B64URL_RE = re.compile(r"^[A-Za-z0-9_-]+={0,2}$")


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def b64url_decode(s):
    return base64.urlsafe_b64decode(s + "=" * (-len(s) % 4))


def write_json_atomic(path, obj):
    d = os.path.dirname(path)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".tmp-")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f, separators=(",", ":"))
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o600)
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def read_json(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


#--------------------------------------------------------------------------------------------------
# Todos

def parse_due(tags):
    """(epoch seconds, all_day, display string) from a due: tag, or None."""
    for t in tags:
        m = DUE_RE.match(t)
        if not m:
            continue
        y, mo, d, hh, mi = m.groups()
        try:
            if hh is None:
                when = dt.datetime(int(y), int(mo), int(d), 9, 0)
                return when.timestamp(), True, f"{y}-{mo}-{d}"
            when = dt.datetime(int(y), int(mo), int(d), int(hh), int(mi))
            return when.timestamp(), False, f"{y}-{mo}-{d}T{hh}:{mi}"
        except ValueError:
            return None
    return None


def title_of(j):
    s = j.get("summary") or j.get("name") or j.get("snippet") or "(untitled)"
    s = re.sub(r"^(\s*\[[^\]]+\])+\s*", "", s)   # a snippet still carries the [todo] brackets
    s = " ".join(s.split())
    return s[:117] + "..." if len(s) > 120 else s


def fetch_todos(loom_url):
    q = urllib.parse.urlencode({"tag": "todo", "notag": "status:done", "brief": "1", "limit": "1000"})
    req = urllib.request.Request(f"{loom_url.rstrip('/')}/jots?{q}",
                                 # Polling is watching, not using: the activity meter leaves
                                 # dashboard-labelled traffic out, and this is the same kind.
                                 headers={"X-Loom-Client": "dashboard"})
    with urllib.request.urlopen(req, timeout=10) as r:
        body = json.load(r)
    todos = []
    for j in body.get("jots", []):
        tags = j.get("tags") or []
        if "for:agent" in tags:
            continue
        due = parse_due(tags)
        if not due:
            continue
        prio = next((t.split(":", 1)[1] for t in tags if t in ("priority:high", "priority:normal", "priority:low")), None)
        todos.append({"id": int(j["id"]), "title": title_of(j), "due": due[0], "all_day": due[1],
                      "due_str": due[2], "priority": prio})
    return todos


# The known-places line format, the same rule as persist/Places.h: "Home: 47.61012,-122.34211 150 m".
PLACE_RE = re.compile(r"^\s*([^:#][^:]{0,39}):\s*(-?\d+(?:\.\d+)?)\s*,\s*(-?\d+(?:\.\d+)?)(?:\s+(\d+(?:\.\d+)?))?")
MAX_PLACES = 50


def parse_places(text):
    out = []
    for line in text.splitlines():
        m = PLACE_RE.match(line)
        if not m:
            continue
        name = m.group(1).strip()[:40]
        lat, lon = float(m.group(2)), float(m.group(3))
        r = float(m.group(4)) if m.group(4) else 150.0
        if name and -90 <= lat <= 90 and -180 <= lon <= 180 and 0 < r <= 50000:
            out.append({"name": name, "lat": lat, "lon": lon, "r": r})
    return out[:MAX_PLACES]


def fetch_places(loom_url):
    """Named places, or [] - a missing or unreadable list must not stop the reminders."""
    req = urllib.request.Request(f"{loom_url.rstrip('/')}/jots/by-name/known-places",
                                 headers={"X-Loom-Client": "dashboard"})
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return parse_places(json.load(r).get("text") or "")
    except Exception:
        return []


def offsets_for(lead, all_day):
    offs = []
    if lead >= 14 * D:
        offs.append(("week", 7 * D))
    if lead >= 2 * D:
        offs.append(("day", D))
    if lead > H and not all_day:
        offs.append(("hour", H))
    elif not all_day:
        # Too close for an hour's notice ("remind me at 5:40"), so remind at the time itself - however
        # close: "due in a minute" is exactly the jot that most needs the nudge.
        offs.append(("due", 0))
    return offs


def grace_for(name):
    """How late a reminder may still go out. A new jot takes a while to reach Loom and be triaged,
    so an at-due reminder for something due a minute after it was written is first seen late; ten
    minutes late is still worth sending. The early reminders keep the 2-minute timer slack."""
    return 600 if name == "due" else 120


#--------------------------------------------------------------------------------------------------
# Wording

def when_text(todo, now):
    due = dt.datetime.fromtimestamp(todo["due"])
    today = dt.datetime.fromtimestamp(now).date()
    days = (due.date() - today).days
    clock = "" if todo["all_day"] else due.strftime(" %H:%M")
    if days < 0:
        return "Overdue since " + due.strftime("%a %b ") + str(due.day)
    if days == 0 and todo["due"] < now:
        return "Overdue since" + (clock or " this morning")
    if days == 0:
        return "Due today" + clock
    if days == 1:
        return "Due tomorrow" + clock
    if days < 7:
        return "Due " + due.strftime("%A") + clock
    return "Due " + due.strftime("%a %b ") + str(due.day) + clock


#--------------------------------------------------------------------------------------------------
# Phone actions

def mark_done(loom_url, tid):
    """Add status:done to an open todo in Loom - todo stays, the pair is the record of the work.
    The one write this process makes to Loom, and only for an id already known to be an open todo
    with a due date. Guarded by expect_updated, so a concurrent edit is reported, not overwritten."""
    base = loom_url.rstrip("/")
    with urllib.request.urlopen(f"{base}/jots/{tid}", timeout=10) as r:
        j = json.load(r)
    tags = j.get("tags") or []
    if "todo" not in tags or "status:done" in tags:
        return
    expect = j.get("updated") or j["id"]
    req = urllib.request.Request(f"{base}/jots/{tid}?expect_updated={expect}", method="PATCH",
                                 data=json.dumps({"tags": tags + ["status:done"]}).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=10):
        pass


def take_actions(notify_dir, state, open_ids, now, loom_url=None):
    """Apply snooze/mute/done lines posted through jot. Every field is validated here.
    Returns the ids marked done, so the caller can drop them from this run."""
    done = set()
    state.pop("test_requested", None)
    path = os.path.join(notify_dir, "actions.jsonl")
    work = path + ".processing"
    if not os.path.exists(work):
        try:
            os.rename(path, work)   # jot reopens actions.jsonl per request, so new lines go to a new file
        except FileNotFoundError:
            return done
    try:
        with open(work, errors="replace") as f:
            lines = f.readlines()[:MAX_ACTIONS]
    finally:
        os.unlink(work)

    for line in lines:
        try:
            a = json.loads(line)
        except ValueError:
            continue
        if not isinstance(a, dict):
            continue
        tid, action = a.get("id"), a.get("action")
        if action == "test":   # the settings sheet's "Send a test" - one per run at most
            state["test_requested"] = True
            continue
        if not isinstance(tid, int) or isinstance(tid, bool) or tid not in open_ids:
            continue
        st = state["todos"].setdefault(str(tid), {})
        if action == "mute":
            st["muted"] = True
            st.pop("snooze_until", None)
        elif action == "unmute":
            st["muted"] = False
        elif action == "snooze":
            until = a.get("until")
            if isinstance(until, (int, float)) and not isinstance(until, bool) and now < until <= now + 31 * D:
                st["snooze_until"] = int(until)
        elif action == "done" and loom_url and tid not in done:
            try:
                mark_done(loom_url, tid)
                done.add(tid)
            except (OSError, ValueError, KeyError) as e:
                log(f"jotnotify: could not mark {tid} done: {e}")
    return done


#--------------------------------------------------------------------------------------------------
# Push

def load_subscription(notify_dir):
    sub = read_json(os.path.join(notify_dir, "subscription.json"), None)
    if not isinstance(sub, dict):
        return None
    ep, keys = sub.get("endpoint"), sub.get("keys")
    if not isinstance(ep, str) or not isinstance(keys, dict) or len(ep) > 1024:
        return None
    u = urllib.parse.urlsplit(ep)
    if (u.scheme != "https" or u.hostname not in ALLOWED_PUSH_HOSTS or u.port not in (None, 443)
            or u.username or u.password):
        log("jotnotify: ignoring a subscription that does not point at an allowed push service")
        return None
    p256dh, auth = keys.get("p256dh"), keys.get("auth")
    try:
        if not (isinstance(p256dh, str) and isinstance(auth, str) and B64URL_RE.match(p256dh)
                and B64URL_RE.match(auth)):
            return None
        pk, ak = b64url_decode(p256dh), b64url_decode(auth)
    except ValueError:
        return None
    if len(pk) != 65 or pk[0] != 4 or len(ak) != 16:
        return None
    return {"endpoint": ep, "keys": {"p256dh": p256dh, "auth": auth}}


class Pusher:
    def __init__(self, args, sub, token):
        self.args, self.sub, self.token = args, sub, token
        self.dead = False

    def link(self, **params):
        return self.args.page_url + "?" + urllib.parse.urlencode(dict(k=self.token, **params))

    def send(self, title, body, link, badge):
        payload = {"web_push": 8030,
                   "notification": {"title": title, "body": body, "navigate": link, "lang": "en"},
                   "app_badge": badge}
        if self.args.dry_run or not self.sub:
            log(f"jotnotify: {'(dry run) ' if self.args.dry_run else '(no subscription) '}{title} | {body}")
            return
        import requests
        from pywebpush import webpush, WebPushException
        session = requests.Session()
        session.max_redirects = 0   # a push service has no reason to redirect, so never follow one
        try:
            webpush(subscription_info=self.sub, data=json.dumps(payload),
                    vapid_private_key=self.args.key, vapid_claims={"sub": self.args.contact},
                    ttl=4 * H, headers={"Urgency": "high"}, timeout=10, requests_session=session)
        except WebPushException as e:
            code = getattr(e.response, "status_code", None)
            if code in (404, 410):
                # The phone dropped this subscription. The page sends a fresh one whenever it opens.
                log("jotnotify: subscription gone (%s), removing it" % code)
                self.dead = True
                self.sub = None
            else:
                log(f"jotnotify: push failed: {e}")


#--------------------------------------------------------------------------------------------------

def run(args):
    now = time.time()
    state_path = os.path.join(args.state_dir, "state.json")
    state = read_json(state_path, {})
    state.setdefault("todos", {})

    todos = fetch_todos(args.loom)
    done = take_actions(args.notify_dir, state, {t["id"] for t in todos}, now, args.loom)
    todos = [t for t in todos if t["id"] not in done]
    by_id = {t["id"]: t for t in todos}

    with open(args.token_file) as f:
        token = f.read().strip()
    sub = load_subscription(args.notify_dir)
    pusher = Pusher(args, sub, token)

    today = dt.datetime.fromtimestamp(now).date()
    def due_today_or_late(t):
        return dt.datetime.fromtimestamp(t["due"]).date() <= today
    badge = sum(1 for t in todos if due_today_or_late(t) and not state["todos"].get(str(t["id"]), {}).get("muted"))

    if state.pop("test_requested", False):
        pusher.send("Test from Loom", "Reminders reach this phone.", pusher.link(), badge)

    for t in todos:
        key = str(t["id"])
        st = state["todos"].get(key, {})
        if st.get("due") != t["due_str"]:
            # New, or its due date moved: re-arm, keeping a mute. Anything already in the past is
            # skipped (a 2-minute grace covers the timer's granularity).
            keep = {k: st[k] for k in ("muted", "snooze_until") if k in st}
            st = dict(keep, due=t["due_str"], sent=[])
            lead = t["due"] - t["id"] / 1e6
            for name, off in offsets_for(lead, t["all_day"]):
                if t["due"] - off <= now - grace_for(name):
                    st["sent"].append(name)
        state["todos"][key] = st

        if st.get("muted"):
            continue
        lead = t["due"] - t["id"] / 1e6
        pending = [n for n, off in offsets_for(lead, t["all_day"])
                   if t["due"] - off <= now and n not in st["sent"]]
        snooze = st.get("snooze_until")
        if snooze and now < snooze:
            continue
        # A snooze ending fires even past the due time - being asked to remind later is a promise.
        # A scheduled reminder does not, beyond its grace: once it is due, the summary and the badge
        # carry it.
        if snooze or (pending and t["due"] + max(grace_for(n) for n in pending) > now):
            pusher.send(t["title"], when_text(t, now), pusher.link(r=t["id"]), badge)
        st["sent"].extend(pending)
        st.pop("snooze_until", None)

    # Morning summary, once a day.
    local = dt.datetime.fromtimestamp(now)
    if state.get("digest") != today.isoformat() and DIGEST_HOUR <= local.hour < DIGEST_LATEST_HOUR:
        state["digest"] = today.isoformat()
        todays = sorted((t for t in todos if dt.datetime.fromtimestamp(t["due"]).date() == today
                         and not state["todos"].get(str(t["id"]), {}).get("muted")), key=lambda t: t["due"])
        if todays:
            lines = [("" if t["all_day"] else dt.datetime.fromtimestamp(t["due"]).strftime("%H:%M ")) + t["title"]
                     for t in todays[:6]]
            if len(todays) > 6:
                lines.append(f"and {len(todays) - 6} more")
            pusher.send(f"{len(todays)} due today", "\n".join(lines), pusher.link(v="today"), badge)

    if pusher.dead:
        try:
            os.unlink(os.path.join(args.notify_dir, "subscription.json"))
        except FileNotFoundError:
            pass

    # Forget todos that are no longer open.
    for key in list(state["todos"]):
        if int(key) not in by_id:
            del state["todos"][key]

    lo, hi = now - OVERDUE_DAYS * D, now + UPCOMING_DAYS * D
    upcoming = []
    for t in sorted(todos, key=lambda t: t["due"]):
        if not lo <= t["due"] <= hi:
            continue
        st = state["todos"].get(str(t["id"]), {})
        upcoming.append({"id": t["id"], "title": t["title"], "due": t["due_str"], "all_day": t["all_day"],
                         "priority": t["priority"], "muted": bool(st.get("muted")),
                         "snoozed_until": st.get("snooze_until")})
    write_json_atomic(os.path.join(args.notify_dir, "upcoming.json"),
                      {"v": 1, "generated": int(now), "vapid_key": public_key_b64(args.key),
                       "subscribed": pusher.sub is not None, "todos": upcoming,
                       "places": fetch_places(args.loom)})
    write_json_atomic(state_path, state)


def public_key_b64(key_path):
    """The applicationServerKey the page subscribes with: the raw uncompressed P-256 point."""
    from cryptography.hazmat.primitives import serialization
    with open(key_path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    raw = key.public_key().public_bytes(serialization.Encoding.X962,
                                        serialization.PublicFormat.UncompressedPoint)
    return base64.urlsafe_b64encode(raw).rstrip(b"=").decode()


def init_key(key_path):
    if os.path.exists(key_path):
        print(f"{key_path} already exists; not replacing it (a new key would invalidate the phone's subscription)")
        return
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric import ec
    key = ec.generate_private_key(ec.SECP256R1())
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    os.makedirs(os.path.dirname(key_path), exist_ok=True)
    fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    print(f"wrote {key_path}")


def main():
    p = argparse.ArgumentParser(description="Phone reminders for Loom todos")
    p.add_argument("--loom", default="http://127.0.0.1:7700", help="Loom base URL")
    p.add_argument("--notify-dir", default="/mnt/fast/web/jot_notify", help="shared with jot --notify-dir")
    p.add_argument("--state-dir", default=os.path.expanduser("~/.local/state/jotnotify"))
    p.add_argument("--key", default=os.path.expanduser("~/.config/loom/vapid_private.pem"),
                   help="VAPID private key (PEM); create it with --init-key")
    p.add_argument("--token-file", default="/etc/loom/jot.token", help="jot's token, for the links in notifications")
    p.add_argument("--page-url", required=False, help="public URL of the capture page, e.g. https://example.com/jot/m")
    p.add_argument("--contact", help="VAPID contact, mailto: or https: (default: the page's origin)")
    p.add_argument("--dry-run", action="store_true", help="log what would be sent instead of sending")
    p.add_argument("--init-key", action="store_true", help="create the VAPID key and exit")
    args = p.parse_args()

    if args.init_key:
        init_key(args.key)
        return
    if not args.page_url:
        p.error("--page-url is required")
    if not args.contact:
        u = urllib.parse.urlsplit(args.page_url)
        args.contact = f"{u.scheme}://{u.netloc}"
    os.makedirs(args.state_dir, exist_ok=True)
    run(args)


if __name__ == "__main__":
    main()
