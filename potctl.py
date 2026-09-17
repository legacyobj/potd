#!/usr/bin/env python3
"""
potctl - interactive terminal client for potd / HTCPCP.

The client discovers pots from GET /, tea varieties from RFC 7168 Alternates,
and additions from the server when possible.  Built-in variety/addition names
are only fallbacks for older or partially compatible servers.

No third-party Python packages are required.
"""

from __future__ import annotations

import argparse
import curses
import json
import re
import socket
import sys
import time
import xml.dom.minidom
from dataclasses import dataclass
from typing import Any, Dict, List, Optional, Sequence, Tuple


MAX_RESPONSE = 1024 * 1024

# Fallbacks taken from the supplied potd source tree.
FALLBACK_VARIETIES = ["darjeeling", "earl-grey", "peppermint"]
FALLBACK_ADDITIONS = [
    "Cream",
    "Half-and-half",
    "Whole-milk",
    "Part-Skim",
    "Skim",
    "Non-Dairy",
    "Vanilla",
    "Almond",
    "Raspberry",
    "Chocolate",
    "Whisky",
    "Rum",
    "Kahlua",
    "Aquavit",
    "Sugar",
    "Xylitol",
    "Stevia",
]

ADDITION_CATEGORY = {
    "Cream": "milk",
    "Half-and-half": "milk",
    "Whole-milk": "milk",
    "Part-Skim": "milk",
    "Skim": "milk",
    "Non-Dairy": "milk",
    "Vanilla": "syrup",
    "Almond": "syrup",
    "Raspberry": "syrup",
    "Chocolate": "syrup",
    "Whisky": "alcohol",
    "Rum": "alcohol",
    "Kahlua": "alcohol",
    "Aquavit": "alcohol",
    "Sugar": "sugar",
    "Xylitol": "sugar",
    "Stevia": "sugar",
}

MILK_ADDITIONS = {
    "Cream",
    "Half-and-half",
    "Whole-milk",
    "Part-Skim",
    "Skim",
    "Non-Dairy",
}


@dataclass
class Response:
    version: str
    status: int
    reason: str
    headers: Dict[str, str]
    body: bytes
    raw: bytes

    def json(self) -> Optional[Any]:
        if not self.body:
            return None
        try:
            return json.loads(self.body.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError):
            return None

    def body_text(self) -> str:
        return self.body.decode("utf-8", "replace")


class HTCPCPClient:
    def __init__(
        self,
        host: str,
        port: int,
        host_header: Optional[str] = None,
        timeout: float = 4.0,
    ) -> None:
        self.host = host
        self.port = port
        self.host_header = host_header or host
        self.timeout = timeout
        self.last_response: Optional[Response] = None

    def request(
        self,
        method: str,
        target: str,
        *,
        headers: Optional[Dict[str, str]] = None,
        body: Optional[bytes] = None,
        version: str = "HTCPCP/1.0",
    ) -> Response:
        fields: List[Tuple[str, str]] = [
            ("Host", self.host_header),
            ("Connection", "close"),
            ("User-Agent", "potctl/1.0"),
        ]

        if headers:
            fields.extend(headers.items())

        lower_names = {name.lower() for name, _ in fields}
        if body is not None and "content-length" not in lower_names:
            fields.append(("Content-Length", str(len(body))))

        request = [f"{method} {target} {version}\r\n".encode("ascii")]
        for name, value in fields:
            request.append(f"{name}: {value}\r\n".encode("latin-1"))
        request.append(b"\r\n")
        if body is not None:
            request.append(body)

        payload = b"".join(request)
        raw = bytearray()

        with socket.create_connection((self.host, self.port), self.timeout) as sock:
            sock.settimeout(self.timeout)
            sock.sendall(payload)

            while len(raw) < MAX_RESPONSE:
                try:
                    chunk = sock.recv(min(65536, MAX_RESPONSE - len(raw)))
                except socket.timeout:
                    raise TimeoutError("server response timed out")
                if not chunk:
                    break
                raw.extend(chunk)

        if len(raw) >= MAX_RESPONSE:
            raise ValueError("response exceeds client safety limit")

        response = self._parse_response(bytes(raw))
        self.last_response = response
        return response

    @staticmethod
    def _parse_response(raw: bytes) -> Response:
        split = raw.find(b"\r\n\r\n")
        if split < 0:
            raise ValueError("incomplete response headers")

        head = raw[:split].decode("latin-1", "replace")
        body = raw[split + 4 :]
        lines = head.split("\r\n")
        if not lines:
            raise ValueError("empty response")

        match = re.match(r"^(\S+)\s+(\d{3})(?:\s+(.*))?$", lines[0])
        if not match:
            raise ValueError(f"invalid status line: {lines[0]!r}")

        version = match.group(1)
        status = int(match.group(2))
        reason = match.group(3) or ""

        headers: Dict[str, str] = {}
        for line in lines[1:]:
            if ":" not in line:
                continue
            name, value = line.split(":", 1)
            key = name.strip().lower()
            value = value.strip()
            if key in headers:
                headers[key] += ", " + value
            else:
                headers[key] = value

        if "content-length" in headers:
            try:
                expected = int(headers["content-length"])
            except ValueError:
                expected = len(body)
            if expected >= 0:
                body = body[:expected]

        return Response(version, status, reason, headers, body, raw)

    def get_inventory(self) -> List[Dict[str, Any]]:
        response = self.request("GET", "/")
        data = response.json()
        if response.status != 200 or not isinstance(data, dict):
            raise RuntimeError(response_summary(response))
        pots = data.get("pots")
        if not isinstance(pots, list):
            raise RuntimeError("GET / did not return a pots array")
        return [p for p in pots if isinstance(p, dict)]

    def discover_varieties(self, pot_name: str) -> List[str]:
        target = "/" + pot_name.lstrip("/")
        response = self.request(
            "BREW",
            target,
            headers={"Content-Type": "message/teapot"},
            body=b"",
        )

        found: List[str] = []
        data = response.json()
        if isinstance(data, dict):
            alternates = data.get("alternates")
            if isinstance(alternates, list):
                for uri in alternates:
                    if isinstance(uri, str):
                        variety = variety_from_uri(uri, pot_name)
                        if variety and variety not in found:
                            found.append(variety)

        alt_header = response.headers.get("alternates", "")
        for uri in re.findall(
            r'\{\s*"([^"]+)"\s+[^\}]*\{\s*type\s+message/teapot\s*\}\s*\}',
            alt_header,
            re.IGNORECASE,
        ):
            variety = variety_from_uri(uri, pot_name)
            if variety and variety not in found:
                found.append(variety)

        return found or FALLBACK_VARIETIES.copy()

    def discover_additions(self) -> List[str]:
        # A read-only negotiation probe. potd returns 406 plus
        # available_additions for a positive but unknown addition token.
        try:
            response = self.request(
                "GET",
                "/",
                headers={"Accept-Additions": "potctl-probe-addition"},
            )
            data = response.json()
            if isinstance(data, dict):
                values = data.get("available_additions")
                if isinstance(values, list):
                    clean = [str(x) for x in values if isinstance(x, str)]
                    if clean:
                        return clean
        except (OSError, TimeoutError, ValueError, RuntimeError):
            pass
        return FALLBACK_ADDITIONS.copy()

    def brew(
        self,
        target: str,
        media_type: str,
        additions: Sequence[str],
    ) -> Response:
        headers = {"Content-Type": media_type}
        if additions:
            # q defaults to 1. The server's policy chooses at most one item
            # from each known category, so the TUI enforces the same.
            headers["Accept-Additions"] = ", ".join(additions)
        return self.request("BREW", target, headers=headers, body=b"start")

    def stop(self, target: str, media_type: str) -> Response:
        return self.request(
            "BREW",
            target,
            headers={"Content-Type": media_type},
            body=b"stop",
        )

    def brew_coffee_on_teapot(self, target: str) -> Response:
        """Intentionally request coffee from a tea pot to exercise RFC 2324 418."""
        return self.request(
            "BREW",
            target,
            headers={"Content-Type": "message/coffeepot"},
            body=b"start",
        )

    def when(self, target: str) -> Response:
        return self.request("WHEN", target)

    def propfind(self, target: str) -> Response:
        return self.request("PROPFIND", target)


def variety_from_uri(uri: str, pot_name: str) -> Optional[str]:
    prefix = "/" + pot_name.strip("/") + "/"
    if not uri.startswith(prefix):
        return None
    value = uri[len(prefix) :]
    if not value or "/" in value:
        return None
    return value


def response_pot(response: Response) -> Optional[Dict[str, Any]]:
    data = response.json()
    if isinstance(data, dict) and isinstance(data.get("pot"), str):
        return data
    return None


def response_summary(response: Response) -> str:
    prefix = f"{response.version} {response.status} {response.reason}".strip()
    data = response.json()
    if isinstance(data, dict):
        if isinstance(data.get("detail"), str):
            return f"{prefix} - {data['detail']}"
        if isinstance(data.get("error"), str):
            return f"{prefix} - {data['error']}"
        if isinstance(data.get("pot"), str):
            state = data.get("state")
            variety = data.get("variety")
            suffix = f"{data['pot']}"
            if state:
                suffix += f" {state}"
            if variety:
                suffix += f" ({variety})"
            return f"{prefix} - {suffix}"
    return prefix


def pretty_response(response: Response) -> str:
    content_type = response.headers.get("content-type", "")
    body = response.body_text()

    if "json" in content_type:
        data = response.json()
        if data is not None:
            body = json.dumps(data, indent=2, ensure_ascii=False)
    elif "xml" in content_type and body.strip():
        try:
            body = xml.dom.minidom.parseString(response.body).toprettyxml(indent="  ")
        except Exception:
            pass

    selected_headers = []
    for name in ("server", "content-type", "safe", "allow", "alternates"):
        if name in response.headers:
            selected_headers.append(f"{name.title()}: {response.headers[name]}")

    parts = [f"{response.version} {response.status} {response.reason}"]
    parts.extend(selected_headers)
    if body.strip():
        parts.extend(["", body.rstrip()])
    return "\n".join(parts)


def safe_addstr(
    win: "curses._CursesWindow",
    y: int,
    x: int,
    text: str,
    attr: int = 0,
) -> None:
    height, width = win.getmaxyx()
    if y < 0 or y >= height or x >= width:
        return
    limit = max(0, width - x - 1)
    if limit <= 0:
        return
    try:
        win.addnstr(y, x, text, limit, attr)
    except curses.error:
        pass


def centered_title(stdscr: "curses._CursesWindow", title: str) -> None:
    _, width = stdscr.getmaxyx()
    x = max(0, (width - len(title)) // 2)
    safe_addstr(stdscr, 0, x, title, curses.A_BOLD)


def choose_menu(
    stdscr: "curses._CursesWindow",
    title: str,
    items: Sequence[str],
    *,
    subtitle: str = "",
) -> Optional[int]:
    if not items:
        return None

    index = 0
    top = 0
    old_timeout = 200
    stdscr.timeout(-1)

    try:
        while True:
            stdscr.erase()
            centered_title(stdscr, title)
            height, width = stdscr.getmaxyx()

            if subtitle:
                safe_addstr(stdscr, 2, 2, subtitle)

            list_y = 4
            visible = max(1, height - list_y - 2)
            if index < top:
                top = index
            elif index >= top + visible:
                top = index - visible + 1

            for row, item_index in enumerate(range(top, min(len(items), top + visible))):
                marker = ">" if item_index == index else " "
                attr = curses.A_REVERSE if item_index == index else 0
                safe_addstr(stdscr, list_y + row, 2, f"{marker} {items[item_index]}", attr)

            safe_addstr(
                stdscr,
                height - 1,
                1,
                "Up/Down select  Enter confirm  Esc cancel",
                curses.A_DIM,
            )
            stdscr.refresh()

            key = stdscr.getch()
            if key in (curses.KEY_UP, ord("k")):
                index = (index - 1) % len(items)
            elif key in (curses.KEY_DOWN, ord("j")):
                index = (index + 1) % len(items)
            elif key in (10, 13, curses.KEY_ENTER):
                return index
            elif key in (27, ord("q")):
                return None
            elif key == curses.KEY_RESIZE:
                continue
    finally:
        stdscr.timeout(old_timeout)


def pick_additions(
    stdscr: "curses._CursesWindow",
    additions: Sequence[str],
    *,
    variety: Optional[str] = None,
) -> Optional[List[str]]:
    values = list(dict.fromkeys(additions))
    blocked: set[str] = set()
    if variety == "peppermint":
        blocked = MILK_ADDITIONS.intersection(values)

    index = 0
    top = 0
    selected: set[str] = set()
    stdscr.timeout(-1)

    try:
        while True:
            stdscr.erase()
            centered_title(stdscr, "Select additions")
            height, _ = stdscr.getmaxyx()

            note = "Space toggles. One selection per known category."
            if blocked:
                note += " Milk is unavailable with peppermint."
            safe_addstr(stdscr, 2, 2, note)

            list_y = 4
            visible = max(1, height - list_y - 3)
            if index < top:
                top = index
            elif index >= top + visible:
                top = index - visible + 1

            for row, item_index in enumerate(range(top, min(len(values), top + visible))):
                name = values[item_index]
                is_blocked = name in blocked
                check = "x" if name in selected else " "
                category = ADDITION_CATEGORY.get(name)
                suffix = f" [{category}]" if category else ""
                if is_blocked:
                    suffix += " [unavailable]"
                attr = curses.A_REVERSE if item_index == index else 0
                if is_blocked:
                    attr |= curses.A_DIM
                safe_addstr(
                    stdscr,
                    list_y + row,
                    2,
                    f"{'>' if item_index == index else ' '} [{check}] {name}{suffix}",
                    attr,
                )

            safe_addstr(
                stdscr,
                height - 2,
                1,
                "Space toggle  Enter brew  c clear  Esc cancel",
                curses.A_DIM,
            )
            if selected:
                safe_addstr(
                    stdscr,
                    height - 1,
                    1,
                    "Selected: " + ", ".join(sorted(selected)),
                )
            else:
                safe_addstr(stdscr, height - 1, 1, "Selected: none")

            stdscr.refresh()
            key = stdscr.getch()

            if key in (curses.KEY_UP, ord("k")):
                index = (index - 1) % len(values)
            elif key in (curses.KEY_DOWN, ord("j")):
                index = (index + 1) % len(values)
            elif key == ord(" "):
                name = values[index]
                if name in blocked:
                    curses.beep()
                    continue
                if name in selected:
                    selected.remove(name)
                    continue

                category = ADDITION_CATEGORY.get(name)
                if category:
                    for current in list(selected):
                        if ADDITION_CATEGORY.get(current) == category:
                            selected.remove(current)
                selected.add(name)
            elif key in (ord("c"), ord("C")):
                selected.clear()
            elif key in (10, 13, curses.KEY_ENTER):
                return [name for name in values if name in selected]
            elif key in (27, ord("q")):
                return None
            elif key == curses.KEY_RESIZE:
                continue
    finally:
        stdscr.timeout(200)


def show_text(
    stdscr: "curses._CursesWindow",
    title: str,
    text: str,
) -> None:
    lines = text.splitlines() or [""]
    top = 0
    stdscr.timeout(-1)

    try:
        while True:
            stdscr.erase()
            centered_title(stdscr, title)
            height, _ = stdscr.getmaxyx()
            visible = max(1, height - 3)

            for row, line in enumerate(lines[top : top + visible]):
                safe_addstr(stdscr, 2 + row, 1, line)

            safe_addstr(
                stdscr,
                height - 1,
                1,
                "Up/Down scroll  PgUp/PgDn page  Esc return",
                curses.A_DIM,
            )
            stdscr.refresh()
            key = stdscr.getch()

            if key in (curses.KEY_UP, ord("k")):
                top = max(0, top - 1)
            elif key in (curses.KEY_DOWN, ord("j")):
                top = min(max(0, len(lines) - visible), top + 1)
            elif key == curses.KEY_PPAGE:
                top = max(0, top - visible)
            elif key == curses.KEY_NPAGE:
                top = min(max(0, len(lines) - visible), top + visible)
            elif key in (27, ord("q"), 10, 13):
                return
            elif key == curses.KEY_RESIZE:
                continue
    finally:
        stdscr.timeout(200)


class TUI:
    def __init__(self, stdscr: "curses._CursesWindow", client: HTCPCPClient, refresh: float) -> None:
        self.stdscr = stdscr
        self.client = client
        self.refresh_interval = max(0.5, refresh)
        self.pots: List[Dict[str, Any]] = []
        self.index = 0
        self.connection_status = "connecting"
        self.action_status = "no action yet"
        self.last_refresh = 0.0
        self.running = True
        self.additions_cache: Optional[List[str]] = None

    def selected(self) -> Optional[Dict[str, Any]]:
        if not self.pots:
            return None
        self.index = min(self.index, len(self.pots) - 1)
        return self.pots[self.index]

    def refresh(self, force: bool = False) -> None:
        now = time.monotonic()
        if not force and now - self.last_refresh < self.refresh_interval:
            return
        try:
            self.pots = self.client.get_inventory()
            self.connection_status = "connected"
            if self.pots:
                self.index = min(self.index, len(self.pots) - 1)
        except Exception as exc:
            self.connection_status = f"refresh failed: {exc}"
        finally:
            self.last_refresh = now

    def update_pot_from_response(self, response: Response) -> Optional[str]:
        data = response_pot(response)
        if not data:
            return None
        name = data.get("pot")
        if not isinstance(name, str):
            return None
        for i, current in enumerate(self.pots):
            if current.get("pot") == name:
                merged = dict(current)
                merged.update(data)
                self.pots[i] = merged
                return str(data.get("state") or "")
        return str(data.get("state") or "")

    def inventory_state(self, pot_name: str) -> Optional[str]:
        for pot in self.pots:
            if pot.get("pot") == pot_name:
                value = pot.get("state")
                return str(value) if value is not None else None
        return None

    def run(self) -> None:
        curses.curs_set(0)
        self.stdscr.keypad(True)
        self.stdscr.timeout(200)
        self.refresh(force=True)

        while self.running:
            self.refresh()
            self.draw()
            key = self.stdscr.getch()
            self.handle_key(key)

    def draw(self) -> None:
        self.stdscr.erase()
        height, width = self.stdscr.getmaxyx()

        title = (
            f"potctl  {self.client.host}:{self.client.port}  "
            f"Host={self.client.host_header}"
        )
        safe_addstr(self.stdscr, 0, 1, title, curses.A_BOLD)
        safe_addstr(self.stdscr, 1, 1, f"network: {self.connection_status}")
        safe_addstr(self.stdscr, 2, 1, f"last action: {self.action_status}")

        header = "  POT       KIND    STATE      VARIETY       STRENGTH   TIME          ADDITIONS"
        safe_addstr(self.stdscr, 4, 1, header, curses.A_UNDERLINE)

        max_rows = max(0, height - 9)
        start = 0
        if self.index >= max_rows and max_rows:
            start = self.index - max_rows + 1

        for row, pot_index in enumerate(range(start, min(len(self.pots), start + max_rows))):
            pot = self.pots[pot_index]
            selected = pot_index == self.index
            marker = ">" if selected else " "
            name = str(pot.get("pot", "?"))
            kind = str(pot.get("kind", "?"))
            state = str(pot.get("state", "?"))
            variety = str(pot.get("variety") or "-")
            strength = pot.get("strength_percent", 0)
            elapsed = pot.get("brew_elapsed_ms", 0)
            recommended = pot.get("recommended_ms", 0)
            batches = pot.get("batches", 0)
            additions = pot.get("additions")
            if not isinstance(additions, list):
                additions = []

            line = (
                f"{marker} {name:<9} {kind:<7} {state:<10} {variety:<13} "
                f"{str(strength) + '%':<10} "
                f"{format_ms(elapsed):>5}/{format_ms(recommended):<5} "
                f"{', '.join(map(str, additions)) or '-'}"
            )
            attr = curses.A_REVERSE if selected else 0
            safe_addstr(self.stdscr, 5 + row, 1, line, attr)

            # Small progress line when actively brewing.
            if selected and state == "brewing" and width >= 60:
                try:
                    ratio = min(1.0, max(0.0, float(elapsed) / max(1.0, float(recommended))))
                except (TypeError, ValueError):
                    ratio = 0.0
                bar_width = min(30, max(10, width - 28))
                filled = int(bar_width * ratio)
                bar = "[" + "#" * filled + "-" * (bar_width - filled) + "]"
                safe_addstr(self.stdscr, height - 4, 1, f"brew {bar}  batches={batches}")

        if not self.pots:
            safe_addstr(self.stdscr, 6, 3, "No pot inventory available.")

        safe_addstr(
            self.stdscr,
            height - 2,
            1,
            "Arrows select  b brew  c coffee->teapot  s stop  w WHEN  p PROPFIND",
            curses.A_DIM,
        )
        safe_addstr(
            self.stdscr,
            height - 1,
            1,
            "Enter actions  r refresh  v last response  ? help  q quit",
            curses.A_DIM,
        )
        self.stdscr.refresh()

    def handle_key(self, key: int) -> None:
        if key == -1:
            return
        if key in (ord("q"), ord("Q")):
            self.running = False
        elif key in (curses.KEY_UP, ord("k")) and self.pots:
            self.index = (self.index - 1) % len(self.pots)
        elif key in (curses.KEY_DOWN, ord("j")) and self.pots:
            self.index = (self.index + 1) % len(self.pots)
        elif key in (ord("r"), ord("R")):
            self.refresh(force=True)
        elif key in (ord("b"), ord("B")):
            self.action_brew()
        elif key in (ord("c"), ord("C")):
            self.action_coffee_on_teapot()
        elif key in (ord("s"), ord("S")):
            self.action_stop()
        elif key in (ord("w"), ord("W")):
            self.action_when()
        elif key in (ord("p"), ord("P")):
            self.action_propfind()
        elif key in (ord("v"), ord("V")):
            self.action_last_response()
        elif key in (ord("?"),):
            self.action_help()
        elif key in (10, 13, curses.KEY_ENTER):
            self.action_menu()
        elif key == curses.KEY_RESIZE:
            pass

    def action_menu(self) -> None:
        pot = self.selected()
        if not pot:
            return

        actions = [
            ("Brew / start", self.action_brew),
        ]

        if str(pot.get("kind", "")) == "tea":
            actions.append(
                ("Brew coffee on teapot", self.action_coffee_on_teapot)
            )

        actions.extend(
            [
                ("Stop now", self.action_stop),
                ("WHEN", self.action_when),
                ("PROPFIND metadata", self.action_propfind),
                ("Show last response", self.action_last_response),
                ("Refresh", lambda: self.refresh(force=True)),
            ]
        )

        choice = choose_menu(
            self.stdscr,
            f"Actions - {pot.get('pot', '?')}",
            [label for label, _ in actions],
            subtitle=(
                f"{pot.get('kind', '?')}  state={pot.get('state', '?')}  "
                f"variety={pot.get('variety') or '-'}"
            ),
        )
        if choice is None:
            return

        actions[choice][1]()

    def action_brew(self) -> None:
        pot = self.selected()
        if not pot:
            self.action_status = "no pot selected"
            return

        name = str(pot.get("pot", ""))
        kind = str(pot.get("kind", ""))
        if not name:
            return

        try:
            if self.additions_cache is None:
                self.action_status = "discovering additions"
                self.draw()
                self.additions_cache = self.client.discover_additions()

            if kind == "coffee":
                additions = pick_additions(self.stdscr, self.additions_cache)
                if additions is None:
                    self.action_status = "brew cancelled"
                    return
                response = self.client.brew(
                    "/" + name,
                    "message/coffeepot",
                    additions,
                )
            elif kind == "tea":
                self.action_status = f"discovering varieties for {name}"
                self.draw()
                varieties = self.client.discover_varieties(name)
                choice = choose_menu(
                    self.stdscr,
                    f"Tea variety - {name}",
                    varieties,
                    subtitle="Discovered from the server's Alternates response.",
                )
                if choice is None:
                    self.action_status = "brew cancelled"
                    return
                variety = varieties[choice]
                additions = pick_additions(
                    self.stdscr,
                    self.additions_cache,
                    variety=variety,
                )
                if additions is None:
                    self.action_status = "brew cancelled"
                    return
                response = self.client.brew(
                    f"/{name}/{variety}",
                    "message/teapot",
                    additions,
                )
            else:
                self.action_status = f"unsupported pot kind: {kind}"
                return

            response_state = self.update_pot_from_response(response)
            pot_name = name
            summary = response_summary(response)

            # Keep the actual BREW result visible instead of letting the
            # subsequent GET / refresh overwrite it with "connected".
            self.action_status = f"BREW -> {summary}"
            self.draw()

            self.refresh(force=True)
            inventory_state = self.inventory_state(pot_name)

            if 200 <= response.status < 300:
                if response_state and inventory_state and response_state != inventory_state:
                    self.action_status = (
                        f"BREW returned state={response_state}, "
                        f"but GET / reports state={inventory_state}"
                    )
                else:
                    self.action_status = (
                        f"BREW OK: {pot_name} state="
                        f"{inventory_state or response_state or 'unknown'}"
                    )
            else:
                self.action_status = f"BREW failed: {summary}"
        except Exception as exc:
            self.action_status = f"brew failed: {exc}"

    def action_coffee_on_teapot(self) -> None:
        pot = self.selected()
        if not pot:
            self.action_status = "no pot selected"
            return

        name = str(pot.get("pot", ""))
        kind = str(pot.get("kind", ""))

        if kind != "tea":
            self.action_status = f"{name or 'selected pot'} is not a teapot"
            curses.beep()
            return

        target = "/" + name

        try:
            response = self.client.brew_coffee_on_teapot(target)
            summary = response_summary(response)

            if response.status == 418:
                self.action_status = f"{summary}"
            else:
                self.action_status = (
                    f"Coffee-on-teapot returned unexpected status: {summary}"
                )

            # Keep the full 418 response available under 'v'.
            # Refresh the inventory afterward without overwriting the action result.
            self.refresh(force=True)
        except Exception as exc:
            self.action_status = f"coffee-on-teapot failed: {exc}"

    def action_stop(self) -> None:
        pot = self.selected()
        if not pot:
            self.action_status = "no pot selected"
            return

        name = str(pot.get("pot", ""))
        kind = str(pot.get("kind", ""))
        variety = pot.get("variety")

        if kind == "coffee":
            target = "/" + name
            media = "message/coffeepot"
        elif kind == "tea":
            if not variety:
                self.action_status = f"{name}: no active/known tea variety to stop"
                curses.beep()
                return
            target = f"/{name}/{variety}"
            media = "message/teapot"
        else:
            self.action_status = f"unsupported pot kind: {kind}"
            return

        try:
            response = self.client.stop(target, media)
            response_state = self.update_pot_from_response(response)
            self.action_status = f"STOP -> {response_summary(response)}"
            self.refresh(force=True)
            current = self.inventory_state(name)
            if 200 <= response.status < 300:
                self.action_status = (
                    f"STOP OK: {name} state="
                    f"{current or response_state or 'unknown'}"
                )
        except Exception as exc:
            self.action_status = f"stop failed: {exc}"

    def action_when(self) -> None:
        pot = self.selected()
        if not pot:
            self.action_status = "no pot selected"
            return

        name = str(pot.get("pot", ""))
        kind = str(pot.get("kind", ""))
        variety = pot.get("variety")
        target = "/" + name
        if kind == "tea" and variety:
            target += "/" + str(variety)

        try:
            response = self.client.when(target)
            self.update_pot_from_response(response)
            self.action_status = f"WHEN -> {response_summary(response)}"
            self.refresh(force=True)
            current = self.inventory_state(name)
            if 200 <= response.status < 300:
                self.action_status = f"WHEN OK: {name} state={current or 'unknown'}"
        except Exception as exc:
            self.action_status = f"WHEN failed: {exc}"

    def action_propfind(self) -> None:
        pot = self.selected()
        if not pot:
            return
        target = "/" + str(pot.get("pot", ""))
        try:
            response = self.client.propfind(target)
            self.action_status = response_summary(response)
            show_text(self.stdscr, f"PROPFIND {target}", pretty_response(response))
        except Exception as exc:
            self.action_status = f"PROPFIND failed: {exc}"

    def action_last_response(self) -> None:
        response = self.client.last_response
        if not response:
            self.action_status = "no response recorded yet"
            return
        show_text(self.stdscr, "Last response", pretty_response(response))

    def action_help(self) -> None:
        show_text(
            self.stdscr,
            "potctl help",
            """potctl is an interactive client for potd.

Main screen
  Up/Down   select a pot
  b         start brewing
  c         send a coffee BREW request to a selected tea pot
  s         stop the selected pot immediately
  w         send WHEN
  p         request PROPFIND metadata
  r         refresh GET / (does not erase the last action result)
  v         show the last server response
  Enter     open the action menu
  q         quit

Brewing
  Coffee:
    select additions, then BREW /pot-N with message/coffeepot.

  Tea:
    the client first asks the selected tea pot for its Alternates,
    lets you choose a variety, then sends BREW to that variety URI
    with message/teapot.

  Coffee on teapot:
    select a tea pot and press c, or use the action menu entry
    "Brew coffee on teapot".
    The client sends BREW /pot-N with Content-Type: message/coffeepot
    and body "start". A tea-only pot may answer
    418 I'm a teapot.

Additions
  Space toggles an addition.
  For known potd additions, the TUI allows one choice per category,
  matching potd's q/preference selection policy.
  Peppermint disables the known milk additions.

Stop
  The s key does not open a menu.  It immediately sends stop for the
  selected coffee pot or the currently known tea variety.

Discovery
  Pot inventory/state: GET /
  Tea varieties: BREW /pot-N, Content-Type: message/teapot, empty body
  Additions: read-only negotiation probe with fallback to the supplied
  potd source list.
""",
        )


def format_ms(value: Any) -> str:
    try:
        ms = int(value)
    except (TypeError, ValueError):
        return "?"
    if ms < 1000:
        return f"{ms}ms"
    seconds = ms / 1000.0
    if seconds < 60:
        return f"{seconds:.1f}s"
    return f"{seconds / 60.0:.1f}m"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Interactive TUI client for potd / HTCPCP."
    )
    parser.add_argument("host", help="server hostname or IP address")
    parser.add_argument(
        "-p",
        "--port",
        type=int,
        default=80,
        help="TCP port (default: 80)",
    )
    parser.add_argument(
        "--host-header",
        help="Host header value (default: same as host)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=4.0,
        help="socket timeout in seconds (default: 4)",
    )
    parser.add_argument(
        "--refresh",
        type=float,
        default=2.0,
        help="inventory refresh interval in seconds (default: 2)",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    client = HTCPCPClient(
        args.host,
        args.port,
        host_header=args.host_header,
        timeout=args.timeout,
    )

    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("potctl requires an interactive terminal.", file=sys.stderr)
        return 2

    def wrapped(stdscr: "curses._CursesWindow") -> None:
        app = TUI(stdscr, client, args.refresh)
        app.run()

    try:
        curses.wrapper(wrapped)
    except KeyboardInterrupt:
        pass
    except curses.error as exc:
        print(f"terminal error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

