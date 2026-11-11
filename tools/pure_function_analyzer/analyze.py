#!/usr/bin/env python3
"""Evidence-first PPC function candidate analyzer for ReXGlue XEX projects.

The tool deliberately separates collected ReXGlue diagnostics from optional
decoded-image evidence. It never runs codegen and only writes files with
--write plus explicit output paths.
"""

from __future__ import annotations

import argparse
import bisect
import csv
import json
import re
import struct
import sys
import tomllib
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable


BRANCH_RE = re.compile(
    r"^\s*(0x[0-9a-fA-F]{8})\s+from\s+(0x[0-9a-fA-F]{8}):\s+"
    r"([A-Za-z][A-Za-z0-9.]*)\s+(0x[0-9a-fA-F]{8})\s+from\s+(0x[0-9a-fA-F]{8})"
)
IMPORT_RE = re.compile(
    r"^\s*(0x[0-9a-fA-F]{8})\s*\|\s*\d+\s*\|\s*(0x[0-9a-fA-F]{8})\s*\|\s*"
    r"([^|]+?)@(\d+):\s*([^|]+?)\s*\|"
)


def addr(value: Any) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def hx(value: int | None) -> str:
    return "unknown" if value is None else f"0x{value:08X}"


def read_text(path: Path) -> str:
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        return raw.decode("utf-16")
    return raw.decode("utf-8-sig", errors="replace")


@dataclass
class Edge:
    target: int
    source: int
    mnemonic: str


@dataclass
class Candidate:
    address: int
    refs: list[Edge] = field(default_factory=list)
    metadata: dict[str, str] = field(default_factory=dict)
    category: str = "uncertain"
    ctr_target: int | None = None
    import_info: str = ""
    evidence: list[str] = field(default_factory=list)
    auto_override: bool = False


@dataclass
class Region:
    start: int
    end: int
    name: str = "exec"


class ImageEvidence:
    """Flat guest image exported by ReXGlue and its address map."""

    def __init__(self, image_path: Path | None, map_path: Path | None, base_arg: str | None):
        self.data = image_path.read_bytes() if image_path else b""
        self.base = int(base_arg, 0) if base_arg else 0
        self.regions: list[Region] = []
        self.mapped_regions: list[Region] = []
        self._exec_starts: list[int] = []
        self._mapped_starts: list[int] = []
        self.sections: list[dict[str, Any]] = []
        self.image_size = 0
        self.entry_point: int | None = None
        self.pdata: dict[int, int] = {}
        self.imports: dict[int, str] = {}
        self.known_functions: dict[int, int] = {}
        self.pointer_refs: dict[int, list[int]] = defaultdict(list)
        self.constructed_refs: dict[int, list[int]] = defaultdict(list)
        self.pointer_site_targets: dict[int, int] = {}
        self.constructed_site_targets: dict[int, int] = {}
        self.fixpoint_candidates: dict[int, list[str]] = {}
        self.fixpoint_conflicts: list[str] = []
        self.fixpoint_iterations: list[dict[str, Any]] = []
        self.fixpoint_stable = False
        self.reference_sites: list[tuple[int, int, str]] = []
        self.fixpoint_rounds = 0
        if map_path:
            self._load_map(json.loads(map_path.read_text(encoding="utf-8")))
        self.regions.sort(key=lambda r: r.start)
        self.mapped_regions.sort(key=lambda r: r.start)
        self._exec_starts = [r.start for r in self.regions]
        self._mapped_starts = [r.start for r in self.mapped_regions]
        if self.data:
            if self.image_size and len(self.data) != self.image_size:
                raise ValueError(f"image.bin size {len(self.data)} != map image_size {self.image_size}")
            if not self.regions:
                self.regions = [Region(self.base, self.base + len(self.data), "image")]
            if not self.mapped_regions:
                self.mapped_regions = list(self.regions)
            self._index_references()

    def _load_map(self, doc: dict[str, Any]) -> None:
        if not self.base:
            self.base = addr(doc.get("image_base", 0))
        self.image_size = addr(doc.get("image_size", 0))
        if doc.get("entry_point") is not None:
            self.entry_point = addr(doc["entry_point"])
        self.sections = list(doc.get("sections", []))
        for item in self.sections:
            region = Region(addr(item["start"]), addr(item["end"]), item.get("name", "section"))
            self.mapped_regions.append(region)
            if item.get("executable", False):
                self.regions.append(region)
        # Backward-compatible input for the earlier analyzer JSON shape.
        for item in doc.get("executable_regions", []):
            region = Region(addr(item["start"]), addr(item["end"]), item.get("name", "exec"))
            if not any(r.start == region.start and r.end == region.end for r in self.regions):
                self.regions.append(region)
            if not any(r.start == region.start and r.end == region.end for r in self.mapped_regions):
                self.mapped_regions.append(region)
        for item in doc.get("pdata", []):
            self.pdata[addr(item["start"])] = addr(item["size"])
        for item in doc.get("functions", []):
            self.known_functions[addr(item["start"])] = addr(item.get("size", 0))
        for item in doc.get("imports", []):
            if item.get("is_function_thunk") is False:
                continue
            self.imports[addr(item["address"])] = (
                f"{item.get('library', 'import')}@{item.get('ordinal', '?')}:"
                f"{item.get('name') or 'ordinal'}"
            )

    def is_mapped(self, address: int, size: int = 4) -> bool:
        index = bisect.bisect_right(self._mapped_starts, address) - 1
        return index >= 0 and address + size <= self.mapped_regions[index].end

    def in_exec(self, address: int, size: int = 4) -> bool:
        index = bisect.bisect_right(self._exec_starts, address) - 1
        return index >= 0 and address + size <= self.regions[index].end

    def conflicts_known_ranges(self, start: int, size: int) -> list[str]:
        end = start + size
        conflicts = []
        known_ranges = dict(self.pdata)
        known_ranges.update(self.known_functions)
        for func_start, func_size in known_ranges.items():
            if func_size and start < func_start + func_size and func_start < end:
                conflicts.append(f"known function 0x{func_start:08X}-0x{func_start + func_size:08X}")
        return conflicts

    def terminator(self, address: int, max_bytes: int = 0x800) -> tuple[int | None, str]:
        for index in range(max_bytes // 4):
            current = address + index * 4
            word = self.word(current)
            if word is None or not self.in_exec(current):
                break
            if word in (0x4E800020, 0x4E800420):
                return current, "blr" if word == 0x4E800020 else "bctr"
            if word >> 26 == 18 and not (word & 1):
                return current, "b (tail branch)"
        return None, "none in scan window"

    def decode_direct_branch(self, address: int) -> tuple[str, int] | None:
        word = self.word(address)
        if word is None or word >> 26 != 18:
            return None
        li = word & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000
        target = li if word & 2 else (address + li) & 0xFFFFFFFF
        return ("bl" if word & 1 else "b"), target & 0xFFFFFFFF

    def _offset(self, address: int, size: int = 4) -> int | None:
        off = address - self.base
        if off < 0 or off + size > len(self.data) or not self.is_mapped(address, size):
            return None
        return off

    def word(self, address: int) -> int | None:
        off = self._offset(address)
        return struct.unpack_from(">I", self.data, off)[0] if off is not None else None

    @staticmethod
    def _is_lis(word: int) -> bool:
        return (word >> 26) == 15 and ((word >> 16) & 31) == 0

    @staticmethod
    def _is_addi(word: int, reg: int) -> bool:
        return (word >> 26) == 14 and ((word >> 21) & 31) == reg and ((word >> 16) & 31) == reg

    def veneer_ctr(self, address: int) -> int | None:
        words = [self.word(address + i * 4) for i in range(4)]
        if any(w is None for w in words):
            return None
        lis, addi, mtctr, bctr = (int(w) for w in words)
        reg = (lis >> 21) & 31
        if not self._is_lis(lis) or reg != 11 or not self._is_addi(addi, reg):
            return None
        # mtctr RS is bits 21..25; accept the canonical XO/SPR encoding.
        # Ignore the RS field (bits 21..25), then validate it separately.
        if (mtctr & 0xFC00FFFF) != 0x7C0003A6 or ((mtctr >> 21) & 31) != reg:
            return None
        if bctr != 0x4E800420:
            return None
        high = lis & 0xFFFF
        low = addi & 0xFFFF
        signed_low = low - 0x10000 if low & 0x8000 else low
        return ((high << 16) + signed_low) & 0xFFFFFFFF

    def prolog_score(self, address: int) -> tuple[int, list[str]]:
        words = [self.word(address + i * 4) for i in range(6)]
        words = [w for w in words if w is not None]
        score = 0
        why: list[str] = []
        for word in words:
            op, rt, ra, imm = word >> 26, (word >> 21) & 31, (word >> 16) & 31, word & 0xFFFF
            if op == 37 and rt == 1 and ra == 1 and imm & 0x8000:  # stwu r1,-frame(r1)
                score += 2
                why.append("stack-frame prolog stwu r1,-N(r1)")
                break
        if any(w == 0x7C0802A6 for w in words):
            score += 1
            why.append("mflr r0 in entry window")
        if any((w >> 26) == 36 and ((w >> 21) & 31) == 0 and ((w >> 16) & 31) == 1
               and (w & 0xFFFF) in (4, 8, 12) for w in words):
            score += 1
            why.append("saved LR near entry")
        return score, why

    def _index_references(self) -> None:
        # Literal BE32 pointers and common lis/addi or lis/ori address materialization.
        for region in self.mapped_regions:
            first_off = ((region.start - self.base + 3) // 4) * 4
            last_off = min(len(self.data) - 3, region.end - self.base - 3)
            for off in range(first_off, last_off + 1, 4):
                site = self.base + off
                value = struct.unpack_from(">I", self.data, off)[0]
                target = value & 0xFFFFFFFF
                if any(r.start <= target < r.end for r in self.regions):
                    self.pointer_refs[target].append(site)
                    self.pointer_site_targets[site] = target
                    self.reference_sites.append((site, target, "literal_be32"))
                if site + 8 > region.end:
                    continue
                first, second = struct.unpack_from(">II", self.data, off)
                if (first >> 26) != 15:
                    continue
                reg = (first >> 21) & 31
                if ((second >> 21) & 31) != reg or ((second >> 16) & 31) != reg:
                    continue
                op = second >> 26
                if op not in (14, 24):  # addi / ori
                    continue
                high = first & 0xFFFF
                low = second & 0xFFFF
                if op == 14:
                    low = low - 0x10000 if low & 0x8000 else low
                    built = ((high << 16) + low) & 0xFFFFFFFF
                    ref_type = "lis_addi"
                else:
                    built = ((high << 16) | low) & 0xFFFFFFFF
                    ref_type = "lis_ori"
                if any(r.start <= built < r.end for r in self.regions):
                    self.constructed_refs[built].append(site)
                    self.constructed_site_targets[site] = built
                    # Preserve the specific construction form in the fixpoint ledger.
                    self.reference_sites.append((site, built, ref_type))
        # Remove duplicates while retaining the construction type.
        self.reference_sites = sorted(set(self.reference_sites))

    def pointer_fixpoint(self, seeds: Iterable[int]) -> set[int]:
        """Expand likely starts referenced from known function bodies; report only, never overrides."""
        known = set(self.pdata) | set(self.known_functions) | set(seeds)
        sizes = dict(self.pdata)
        sizes.update(self.known_functions)
        ranges = {start: size for start, size in sizes.items() if size > 0}
        range_starts = sorted(ranges)

        def overlapping(start: int, end: int) -> list[int]:
            idx = max(0, bisect.bisect_left(range_starts, start) - 1)
            found = []
            while idx < len(range_starts) and range_starts[idx] < end:
                other = range_starts[idx]
                if start < other + ranges[other] and other < end:
                    found.append(other)
                idx += 1
            return found

        # First pass uses pointer tables and constructed addresses anywhere in mapped
        # sections. Later rounds inspect references inside newly discovered functions.
        # The complete mapped-image sweep below already checks all pointer and
        # address-materialization sites in the initial PDATA/known-function set.
        # Re-scanning every PDATA range in round 1 would revisit almost the whole
        # image tens of thousands of times. Later rounds are seeded only by newly
        # inferred candidate bodies.
        frontier: set[int] = set()
        discovered: set[int] = set()
        self.fixpoint_iterations = []

        def consider(site: int, target: int, ref_type: str, iteration: int) -> bool:
            if target in known or not self.in_exec(target):
                return False
            if any(start < target < start + size for start, size in self.pdata.items() if size):
                return False
            score, prolog_evidence = self.prolog_score(target)
            terminal_at, terminal_kind = self.terminator(target)
            if score < 2 or terminal_at is None:
                return False
            inferred_end = terminal_at + 4
            overlaps = overlapping(target, inferred_end)
            record = {
                "iteration": iteration,
                "candidate": target,
                "source": site,
                "reference_type": ref_type,
                "prolog_score": score,
                "prolog_evidence": prolog_evidence,
                "terminator": terminal_kind,
                "terminator_address": terminal_at,
                "inferred_end": inferred_end,
                "boundary_proof": "prolog score >= 2 plus scanned terminator; heuristic only",
                "conflict": [
                    {"start": start, "size": ranges[start]}
                    for start in overlaps
                ],
            }
            self.fixpoint_iterations.append(record)
            if overlaps:
                desc = (f"iteration {iteration}: 0x{target:08X}-0x{inferred_end:08X} "
                        f"from {ref_type} at 0x{site:08X} overlaps "
                        + ", ".join(f"0x{start:08X}" for start in overlaps))
                self.fixpoint_conflicts.append(desc)
                return False
            discovered.add(target)
            self.fixpoint_candidates.setdefault(target, []).append(
                f"iteration {iteration}; {ref_type} at 0x{site:08X}; "
                f"{'; '.join(prolog_evidence)}; terminator {terminal_kind} at 0x{terminal_at:08X}")
            known.add(target)
            size = inferred_end - target
            sizes[target] = size
            ranges[target] = size
            bisect.insort(range_starts, target)
            if iteration == 0:
                frontier.add(target)
            return True

        refs_by_target: dict[int, list[tuple[int, str]]] = defaultdict(list)
        for site, target, ref_type in self.reference_sites:
            refs_by_target[target].append((site, ref_type))
        for target, references in refs_by_target.items():
            if target in known or not self.in_exec(target):
                continue
            # Evaluate a destination once, while retaining each source/type as
            # auditable provenance if its boundary passes the heuristic.
            first_site, first_type = references[0]
            if consider(first_site, target, first_type, 0):
                for site, ref_type in references[1:]:
                    self.fixpoint_iterations.append({
                        "iteration": 0, "candidate": target, "source": site,
                        "reference_type": ref_type, "prolog_score": 0,
                        "prolog_evidence": [], "terminator": "shared candidate evidence",
                        "terminator_address": None,
                        "inferred_end": target + sizes[target],
                        "boundary_proof": "same destination as candidate accepted in this sweep",
                        "conflict": [],
                    })
                    self.fixpoint_candidates[target].append(
                        f"iteration 0; {ref_type} at 0x{site:08X}; shares candidate boundary evidence")
        rounds = 0
        sites = [entry[0] for entry in self.reference_sites]
        while frontier and rounds < 32:
            rounds += 1
            next_frontier: set[int] = set()
            for owner in frontier:
                span = sizes.get(owner, 0)
                if span <= 0:
                    continue
                end = owner + span
                lo = bisect.bisect_left(sites, owner)
                hi = bisect.bisect_left(sites, end)
                for ref_site, candidate, ref_type in self.reference_sites[lo:hi]:
                    if consider(ref_site, candidate, ref_type, rounds):
                        next_frontier.add(candidate)
            frontier = next_frontier
        self.fixpoint_rounds = rounds
        self.fixpoint_stable = not frontier
        return discovered


def parse_logs(paths: list[Path]) -> dict[int, Candidate]:
    candidates: dict[int, Candidate] = {}
    for path in paths:
        for line in read_text(path).splitlines():
            match = BRANCH_RE.match(line)
            if not match:
                continue
            target, source, mnemonic, repeated_target, repeated_source = match.groups()
            t, s = addr(target), addr(source)
            if t != addr(repeated_target) or s != addr(repeated_source):
                continue
            candidates.setdefault(t, Candidate(t)).refs.append(Edge(t, s, mnemonic.lower()))
    return candidates


def read_classification(path: Path | None) -> dict[int, dict[str, str]]:
    if path is None:
        return {}
    rows: dict[int, dict[str, str]] = {}
    with path.open("r", encoding="utf-8-sig", newline="") as handle:
        for row in csv.DictReader(handle):
            if not row.get("Stub"):
                continue
            rows[addr(row["Stub"])] = row
    return rows


def read_import_map(path: Path | None) -> dict[int, tuple[int, str]]:
    result: dict[int, tuple[int, str]] = {}
    if path is None:
        return result
    for line in read_text(path).splitlines():
        match = IMPORT_RE.match(line)
        if match:
            stub, ctr, library, ordinal, name = match.groups()
            result[addr(stub)] = (addr(ctr), f"{library}@{ordinal}:{name.strip()}")
    return result


def classify(candidates: dict[int, Candidate], rows: dict[int, dict[str, str]],
             imports: dict[int, tuple[int, str]], image: ImageEvidence) -> None:
    for address, candidate in candidates.items():
        row = rows.get(address)
        if row:
            candidate.metadata = row
            candidate.evidence.append("collected veneer-classification.csv metadata")
        if address in imports and not image.data:
            candidate.ctr_target, candidate.import_info = imports[address]
            candidate.category = "import"
            candidate.evidence.append("CTR target mapped to XEX import table in saved audit")
            continue
        if image.data:
            ctr = image.veneer_ctr(address) if image.in_exec(address, 16) else None
            if ctr is not None:
                candidate.ctr_target = ctr
                candidate.evidence.append("exact PPC lis/addi/mtctr/bctr bytes verified in flat image")
                if address in imports:
                    audited_ctr, audited_name = imports[address]
                    if ctr != audited_ctr:
                        candidate.category = "conflict"
                        candidate.evidence.append(
                            f"saved import audit CTR {hx(audited_ctr)} disagrees with decoded CTR {hx(ctr)}")
                        continue
                    candidate.import_info = audited_name
                    candidate.evidence.append("decoded CTR agrees with saved import audit")
                if ctr in image.imports:
                    candidate.import_info = candidate.import_info or image.imports[ctr]
                    candidate.category = "import"
                    candidate.evidence.append("CTR target matched import map")
                    continue
                if candidate.import_info:
                    candidate.category = "import"
                    candidate.evidence.append("CTR target matched saved import audit; SDK export preserves ordinal metadata")
                    continue
                if ctr in image.pdata or ctr in image.known_functions:
                    conflicts = image.conflicts_known_ranges(address, 16)
                    if conflicts:
                        candidate.category = "conflict"
                        candidate.evidence.extend(conflicts)
                    else:
                        candidate.category = "certain_veneer"
                        candidate.auto_override = True
                        candidate.evidence.append("CTR target is exact known function start")
                    continue
                if any(start < ctr < start + size for start, size in image.pdata.items() if size):
                    candidate.category = "inside_function"
                    candidate.evidence.append("CTR target lies inside PDATA extent")
                    continue
                candidate.category = "uncertain"
                candidate.evidence.append("veneer pattern valid but CTR target has no known boundary")
                continue
            if image.in_exec(address):
                score, why = image.prolog_score(address)
                candidate.evidence.extend(why)
                terminal_at, terminal_kind = image.terminator(address)
                candidate.evidence.append(f"terminator scan: {terminal_kind} at {hx(terminal_at)}")
                if address in image.pdata:
                    candidate.category = "certain_function"
                    candidate.evidence.append("address is an exact PDATA start")
                elif any(start < address < start + size for start, size in image.pdata.items() if size):
                    candidate.category = "inside_function"
                    candidate.evidence.append("address lies inside a PDATA extent")
                elif score >= 2 and terminal_at is not None:
                    candidate.category = "probable_function"
                else:
                    candidate.category = "uncertain"
        elif row:
            cat = row.get("Category", "")
            exec_ok = row.get("Executable", "").lower() == "true"
            no_overlap = row.get("PdataOverlap", "").lower() == "false" and row.get("ImportOverlap", "").lower() == "false"
            if cat == "A" and exec_ok and no_overlap:
                candidate.category = "certain_veneer"
                candidate.auto_override = True
                candidate.evidence.append("prior audit group A: executable, no overlap, CTR at PDATA start")
            elif cat == "B":
                candidate.category = "import"
                candidate.evidence.append("prior audit category B; requires import/target mapping review")
            elif cat == "D":
                candidate.category = "uncertain_veneer"
                candidate.evidence.append("prior audit group D: unknown CTR destination")
            elif cat == "NonVeneer":
                candidate.category = "uncertain_nonveneer"
                candidate.evidence.append("prior audit did not classify as 16-byte veneer")
        else:
            candidate.category = "uncertain"
            candidate.evidence.append("no raw image or cached binary classification available")
    if image.data:
        for candidate in candidates.values():
            mismatches = []
            for edge in candidate.refs:
                decoded = image.decode_direct_branch(edge.source)
                if decoded is None:
                    mismatches.append(f"source {hx(edge.source)} is not a direct b/bl in supplied image")
                elif decoded != (edge.mnemonic, edge.target):
                    mismatches.append(
                        f"source {hx(edge.source)} decodes {decoded[0]}->{hx(decoded[1])}, "
                        f"log says {edge.mnemonic}->{hx(edge.target)}")
            if mismatches:
                candidate.category = "conflict"
                candidate.auto_override = False
                candidate.evidence.extend(mismatches)


def generate_overrides(candidates: dict[int, Candidate]) -> str:
    selected = sorted(c.address for c in candidates.values() if c.auto_override)
    lines = ["# Generated by pure_function_analyzer; review before use.", "# Only high-confidence candidates are included.", "", "[functions]"]
    lines.extend(f'"0x{address:08X}" = {{ size = 16 }}' for address in selected)
    return "\n".join(lines) + "\n"


def make_report(candidates: dict[int, Candidate], all_rows: dict[int, dict[str, str]], image: ImageEvidence,
                log_paths: list[Path], classification_path: Path | None,
                accepted_addresses: set[int]) -> str:
    branch_candidates = [c for c in candidates.values() if c.refs]
    counts = Counter(c.category for c in branch_candidates)
    refs = Counter(c.category for c in branch_candidates for _ in c.refs)
    prior_safe = sum(1 for row in all_rows.values() if row.get("Category") == "A"
                     and row.get("Executable", "").lower() == "true"
                     and row.get("PdataOverlap", "").lower() == "false"
                     and row.get("ImportOverlap", "").lower() == "false")
    lines = [
        "PURE-NX PPC function candidate analysis",
        "========================================",
        f"Logs: {', '.join(str(p) for p in log_paths)}",
        f"Cached classification: {classification_path or 'none'}",
        f"Unresolved edges parsed: {sum(len(c.refs) for c in candidates.values())}",
        f"Unique branch destinations: {len(branch_candidates)}",
        f"Prior audit group-A candidates (already probed historically): {prior_safe}",
        f"Accepted temporary probe overrides supplied: {len(accepted_addresses)} unique functions",
        f"Flat decoded image supplied: {'yes' if image.data else 'no'}",
        f"Image base/size: {hx(image.base)} / 0x{image.image_size:X} bytes",
        f"Image entry point: {hx(image.entry_point)}",
        f"Mapped sections: {len(image.sections)}; executable sections: {len(image.regions)}",
        f"Image-derived PDATA starts: {len(image.pdata)}",
        f"SDK import entries: {len(image.imports)} function thunks",
        f"Pointer index entries: {sum(map(len, image.pointer_refs.values())) + sum(map(len, image.constructed_refs.values()))}",
        f"Pointer-fixpoint rounds: {image.fixpoint_rounds}",
        f"Pointer-fixpoint stable: {'yes' if image.fixpoint_stable else 'no (round limit or pending frontier)'}",
        f"Pointer-fixpoint candidates (review only): {len(image.fixpoint_candidates)}",
        f"Pointer-fixpoint range conflicts: {len(image.fixpoint_conflicts)}",
        "",
        "Current unresolved destinations by class:",
    ]
    for key, value in sorted(counts.items()):
        lines.append(f"  {key}: {value} destinations / {refs[key]} edges")
    lines += ["", "Destination details (sorted by descending references):"]
    for c in sorted(branch_candidates, key=lambda x: (-len(x.refs), x.address)):
        mnemonics = Counter(e.mnemonic for e in c.refs)
        row = c.metadata
        section = row.get("Section", "")
        ctr = c.ctr_target or (addr(row["CTRTarget"]) if row.get("CTRTarget") else None)
        extra = f"; ctr={hx(ctr)}" if ctr is not None else ""
        if c.import_info:
            extra += f"; import={c.import_info}"
        if section:
            extra += f"; prior-section={section}"
        if c.refs and image.data:
            decoded = image.decode_direct_branch(c.refs[0].source)
            if decoded:
                extra += f"; decoded-source={decoded[0]}->{hx(decoded[1])}"
        edge_types = ",".join(f"{k}:{v}" for k, v in sorted(mnemonics.items()))
        lines.append(f"  {hx(c.address)} refs={len(c.refs)} branches=[{edge_types}] class={c.category}{extra}")
    strong = sum(c.category in {"certain_function", "certain_veneer"} for c in branch_candidates)
    uncertain = sum(c.category.startswith("uncertain") or c.category == "probable_function"
                    for c in branch_candidates)
    conflicts = sum(c.category == "conflict" for c in branch_candidates)
    lines += [
        "",
        f"Unresolved-target summary: {len(branch_candidates)} targets; {strong} strongly confirmed; "
        f"{uncertain} uncertain/heuristic; {conflicts} conflicts.",
        f"Additional pointer-fixpoint function candidates: {len(image.fixpoint_candidates)} "
        "(heuristic only; no automatic overrides).",
    ]
    lines += ["", "Evidence / safety:"]
    if accepted_addresses:
        examples = ", ".join(hx(a) for a in sorted(accepted_addresses)[:8])
        lines.append(f"  Accepted probe examples: {examples}")
    selected = [c for c in candidates.values() if c.auto_override]
    lines.append(f"  High-confidence overrides proposed for current unresolved set: {len(selected)}")
    for c in selected:
        lines.append(f"  {hx(c.address)}: {'; '.join(c.evidence)}")
    if image.fixpoint_conflicts:
        lines += ["", "Pointer-fixpoint range conflicts:"]
        lines.extend(f"  {item}" for item in image.fixpoint_conflicts)
    if image.fixpoint_iterations:
        lines += ["", "Pointer-fixpoint iteration ledger:"]
        for item in image.fixpoint_iterations:
            candidate = item["candidate"]
            source = item["source"]
            conflict = item["conflict"]
            proof = item["boundary_proof"]
            evidence = "; ".join(item["prolog_evidence"])
            lines.append(
                f"  iter={item['iteration']} candidate={hx(candidate)} source={hx(source)} "
                f"type={item['reference_type']} prolog={item['prolog_score']} ({evidence}) "
                f"terminator={item['terminator']}@{hx(item['terminator_address'])} "
                f"boundary={proof} conflict={','.join(hx(r['start']) for r in conflict) or 'none'}")
    if not image.data:
        lines += [
            "",
            "Image limitation: raw PPC instructions, pointer locations, and PDATA extents were not re-read in this run.",
            "The CSV facts are treated as prior audit evidence, not independently revalidated. Provide a flat decoded",
            "guest image plus the JSON map schema in README.md for byte-level pointer-fixpoint analysis.",
        ]
    return "\n".join(lines) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", action="append", required=True, help="ReXGlue codegen log; repeatable")
    parser.add_argument("--classification-csv", type=Path, help="Previously audited stub classification CSV")
    parser.add_argument("--analysis-report", type=Path, help="Saved import mapping analysis text")
    parser.add_argument("--accepted-overrides", action="append", default=[],
                        help="Prior probe overrides TOML; repeatable and read-only")
    parser.add_argument("--image", type=Path, help="Flat decoded XEX image (guest bytes, big endian PPC)")
    parser.add_argument("--map", type=Path, help="JSON image map: regions, PDATA, functions, imports")
    parser.add_argument("--base", help="Guest address corresponding to byte zero of --image, e.g. 0x82000000")
    parser.add_argument("--write", action="store_true", help="Write outputs; otherwise this is a dry-run")
    parser.add_argument("--report", type=Path, help="Report output path (required with --write)")
    parser.add_argument("--overrides", type=Path, help="Temporary TOML output path (required with --write)")
    args = parser.parse_args(argv)
    log_paths = [Path(p) for p in args.log]
    for path in log_paths:
        if not path.is_file():
            parser.error(f"log does not exist: {path}")
    if args.write and (not args.report or not args.overrides):
        parser.error("--write requires both --report and --overrides")
    if args.image and not args.image.is_file():
        parser.error(f"image does not exist: {args.image}")
    if args.map and not args.map.is_file():
        parser.error(f"map does not exist: {args.map}")

    candidates = parse_logs(log_paths)
    rows = read_classification(args.classification_csv)
    import_map = read_import_map(args.analysis_report)
    accepted_sizes: dict[int, int] = {}
    for override_path in args.accepted_overrides:
        doc = tomllib.loads(Path(override_path).read_text(encoding="utf-8"))
        accepted_sizes.update({addr(key): addr(value.get("size", 0))
                               for key, value in doc.get("functions", {}).items()})
    image = ImageEvidence(args.image, args.map, args.base)
    image.known_functions.update({a: size for a, size in accepted_sizes.items() if size > 0})
    if image.entry_point is not None:
        image.known_functions.setdefault(image.entry_point, image.pdata.get(image.entry_point, 0))
    accepted_addresses = set(accepted_sizes)
    classify(candidates, rows, import_map, image)
    # Discovery is intentionally evidence-driven. Only inspect pointer chains if a flat image and PDATA map exist.
    if image.data and image.pdata:
        image.pointer_fixpoint(candidates.keys() | accepted_addresses | set(image.imports))
        for address, references in image.fixpoint_candidates.items():
            if address not in candidates:
                candidates[address] = Candidate(
                    address, category="probable_function",
                    evidence=["pointer-fixpoint candidate; not auto-emitted"] + references,
                )
    report = make_report(candidates, rows, image, log_paths, args.classification_csv,
                         accepted_addresses)
    overrides = generate_overrides(candidates)
    if args.write:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.overrides.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(report, encoding="utf-8")
        args.overrides.write_text(overrides, encoding="utf-8")
        print(f"Wrote report: {args.report}")
        print(f"Wrote temporary overrides: {args.overrides}")
    else:
        print(report, end="")
        print("\n--- DRY-RUN proposed temporary overrides (not written) ---")
        print(overrides, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
