#!/usr/bin/env python3
"""Strictly validate pointer-fixpoint candidates from an analyzer report.

This tool reads the SDK-exported PPC image and the analyzer's auditable ledger.
It never runs codegen. It writes a candidate-by-candidate review and a temporary
combined overrides file; only PDATA starts or candidates meeting the strict B
rule are added beyond previously accepted probes.
"""

from __future__ import annotations

import argparse
import bisect
import json
import re
import struct
import tomllib
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any


HX_RE = r"0x[0-9A-Fa-f]{8}"
LEDGER_RE = re.compile(
    rf"iter=(\d+) candidate=({HX_RE}) source=({HX_RE}) type=([^ ]+) "
    rf"prolog=(\d+) \((.*?)\) terminator=(.*?)@({HX_RE}|unknown) "
    rf"boundary=(.*?) conflict=(.*)$"
)
CONFLICT_RE = re.compile(
    rf"iteration (\d+): ({HX_RE})-({HX_RE}) from ([^ ]+) at ({HX_RE}) overlaps (.*)$"
)


def n(value: Any) -> int:
    return value if isinstance(value, int) else int(str(value), 0)


def hx(value: int | None) -> str:
    return "unknown" if value is None else f"0x{value:08X}"


def load_functions(path: Path) -> dict[int, int]:
    doc = tomllib.loads(path.read_text(encoding="utf-8"))
    return {n(key): n(item.get("size", 0)) for key, item in doc.get("functions", {}).items()}


class PPCImage:
    def __init__(self, image_path: Path, map_path: Path):
        self.data = image_path.read_bytes()
        self.doc = json.loads(map_path.read_text(encoding="utf-8"))
        self.base = n(self.doc["image_base"])
        self.end = self.base + n(self.doc["image_size"])
        if len(self.data) != n(self.doc["image_size"]):
            raise ValueError("image.bin size does not match image-map.json image_size")
        self.sections = sorted(
            ({"start_i": n(s["start"]), "end_i": n(s["end"]), **s}
             for s in self.doc["sections"]), key=lambda s: s["start_i"])
        self.section_starts = [s["start_i"] for s in self.sections]
        self.exec_sections = [s for s in self.sections if s.get("executable")]
        self.exec_starts = [s["start_i"] for s in self.exec_sections]
        self.pdata = sorted((n(f["start"]), n(f["start"]) + n(f["size"]), n(f["size"]))
                            for f in self.doc["pdata"])
        self.pdata_starts = [item[0] for item in self.pdata]

    def section(self, address: int, size: int = 1) -> dict[str, Any] | None:
        i = bisect.bisect_right(self.section_starts, address) - 1
        if i >= 0 and address + size <= self.sections[i]["end_i"]:
            return self.sections[i]
        return None

    def executable_section(self, address: int, size: int = 4) -> dict[str, Any] | None:
        i = bisect.bisect_right(self.exec_starts, address) - 1
        if i >= 0 and address + size <= self.exec_sections[i]["end_i"]:
            return self.exec_sections[i]
        return None

    def word(self, address: int) -> int | None:
        if self.section(address, 4) is None:
            return None
        off = address - self.base
        if off < 0 or off + 4 > len(self.data):
            return None
        return struct.unpack_from(">I", self.data, off)[0]

    def pdata_at(self, address: int) -> tuple[int, int, int] | None:
        i = bisect.bisect_right(self.pdata_starts, address) - 1
        if i >= 0 and self.pdata[i][0] <= address < self.pdata[i][1]:
            return self.pdata[i]
        return None

    def pdata_overlaps(self, start: int, end: int) -> list[tuple[int, int, int]]:
        i = max(0, bisect.bisect_left(self.pdata_starts, start) - 1)
        out = []
        while i < len(self.pdata) and self.pdata[i][0] < end:
            item = self.pdata[i]
            if start < item[1] and item[0] < end:
                out.append(item)
            i += 1
        return out

    def owner(self, address: int) -> tuple[int, int, int] | None:
        return self.pdata_at(address)

    @staticmethod
    def decode(word: int | None) -> str:
        if word is None:
            return "<unmapped>"
        if word == 0x4E800020:
            return "blr"
        if word == 0x4E800420:
            return "bctr"
        if word == 0x7C0802A6:
            return "mflr r0"
        op = word >> 26
        rt, ra, rb = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
        imm = word & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 36:
            return f"stw r{rt},{simm}(r{ra})"
        if op == 37:
            return f"stwu r{rt},{simm}(r{ra})"
        if op == 14:
            return f"addi r{rt},r{ra},{simm}"
        if op == 15:
            return f"lis r{rt},{simm}"
        if op == 18:
            li = word & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            return f"{'bl' if word & 1 else 'b'} {li:+#x}"
        if op == 16:
            return "bc (conditional branch)"
        memory_ops = {
            32: "lwz", 33: "lwzu", 34: "lbz", 35: "lbzu",
            38: "stb", 39: "stbu", 40: "lhz", 41: "lhzu",
            42: "lha", 43: "lhau", 44: "sth", 45: "sthu",
            46: "lmw", 47: "stmw",
        }
        if op in memory_ops:
            return f"{memory_ops[op]} r{rt},{simm}(r{ra})"
        if op == 19 and ((word >> 1) & 0x3FF) in (16, 528):
            return "bclr/bcctr"
        if op == 31:
            xo = (word >> 1) & 0x3FF
            if xo == 467:
                return f"mtctr r{rt}"
            if xo == 339:
                return f"mfspr r{rt}"
        return f"op{op} 0x{word:08X}"

    def instructions(self, address: int, count: int = 8) -> list[str]:
        return [f"{hx(address + i * 4)}: {self.decode(self.word(address + i * 4))}"
                for i in range(count)]

    def prolog(self, address: int) -> tuple[int, list[str]]:
        words = [self.word(address + i * 4) for i in range(8)]
        score = 0
        proof: list[str] = []
        if words and words[0] is not None:
            w = words[0]
            op, rt, ra, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 37 and rt == 1 and ra == 1 and imm & 0x8000:
                score += 3
                proof.append("entry stwu r1,-frame(r1)")
        if any(w == 0x7C0802A6 for w in words[:4]):
            score += 2
            proof.append("mflr r0 in first four instructions")
        if any(w is not None and (w >> 26) == 36 and ((w >> 21) & 31) == 0
               and ((w >> 16) & 31) == 1 and ((w & 0xFFFF) <= 0x100)
               for w in words[:8]):
            score += 1
            proof.append("saved LR to stack near entry")
        # A recognized no-frame leaf pattern is useful context but not a boundary proof.
        if words and words[0] is not None and words[0] in (0x4E800020, 0x4E800420):
            proof.append("one-instruction return/thunk-like entry")
        return score, proof

    def veneer(self, address: int) -> int | None:
        words = [self.word(address + i * 4) for i in range(4)]
        if any(w is None for w in words):
            return None
        lis, addi, mtctr, bctr = (int(w) for w in words)
        reg = (lis >> 21) & 31
        if ((lis >> 26) != 15 or ((lis >> 16) & 31) != 0 or reg != 11
                or (addi >> 26) != 14 or ((addi >> 21) & 31) != reg
                or ((addi >> 16) & 31) != reg
                or (mtctr & 0xFC00FFFF) != 0x7C0003A6
                or ((mtctr >> 21) & 31) != reg or bctr != 0x4E800420):
            return None
        hi, lo = lis & 0xFFFF, addi & 0xFFFF
        lo = lo - 0x10000 if lo & 0x8000 else lo
        return ((hi << 16) + lo) & 0xFFFFFFFF

    def terminator(self, address: int, limit: int = 0x200) -> tuple[int | None, str]:
        # Linear scan is corroborating evidence only; PPC control flow is not decoded here.
        for off in range(0, limit, 4):
            current = address + off
            if self.executable_section(current) is None:
                break
            word = self.word(current)
            if word in (0x4E800020, 0x4E800420):
                return current, "blr" if word == 0x4E800020 else "bctr"
            if word is not None and word >> 26 == 18 and not (word & 1):
                return current, "unconditional b (not CFG-verified)"
        return None, "no return/tail branch in 0x200-byte window"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--image", type=Path, required=True)
    ap.add_argument("--map", type=Path, required=True)
    ap.add_argument("--analysis-report", type=Path, required=True)
    ap.add_argument("--classification-report", type=Path, required=True)
    ap.add_argument("--accepted-overrides", type=Path, action="append", required=True)
    ap.add_argument("--output-report", type=Path, required=True)
    ap.add_argument("--output-overrides", type=Path, required=True)
    args = ap.parse_args()

    image = PPCImage(args.image, args.map)
    accepted: dict[int, int] = {}
    for path in args.accepted_overrides:
        accepted.update(load_functions(path))
    # Include the independently verified one-edge veneer from the previous report.
    accepted[0x821F4C40] = 16

    report_text = args.analysis_report.read_text(encoding="utf-8")
    candidates: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for line in report_text.splitlines():
        if "iter=" not in line or " candidate=" not in line:
            continue
        match = LEDGER_RE.search(line)
        if not match:
            continue
        iteration, candidate, source, ref_type, score, prolog, term, term_addr, boundary, conflict = match.groups()
        address = int(candidate, 16)
        candidates[address].append({
            "iteration": int(iteration), "source": int(source, 16),
            "type": ref_type, "reported_score": int(score),
            "reported_prolog": prolog, "reported_terminator": term,
            "reported_terminator_address": None if term_addr == "unknown" else int(term_addr, 16),
            "reported_boundary": boundary, "reported_conflict": conflict,
        })

    conflict_lines: list[dict[str, Any]] = []
    for line in report_text.splitlines():
        match = CONFLICT_RE.search(line)
        if not match:
            continue
        iteration, start, end, ref_type, source, overlaps = match.groups()
        left, right = int(start, 16), int(end, 16)
        overlap_starts = [int(v, 16) for v in re.findall(HX_RE, overlaps)]
        collision_rows = []
        for other in overlap_starts:
            pdata = image.pdata_at(other)
            known = next(((a, z) for a, z in accepted.items() if a <= other < a + z), None)
            collision_rows.append({
                "start": other,
                "range": (pdata[0], pdata[1]) if pdata else
                         ((known[0], known[0] + known[1]) if known else (other, other + 4)),
                "kind": "PDATA" if pdata else ("accepted override" if known else "other overlap"),
            })
        conflict_lines.append({
            "iteration": int(iteration), "start": left, "end": right,
            "ref_type": ref_type, "source": int(source, 16), "collisions": collision_rows,
        })

    # The ledger also prints 16 rejected overlapping guesses. They are not in
    # the analyzer's 1,093 accepted fixpoint candidate set; retain them only in
    # the separate conflict table below.
    candidates = {
        address: [ref for ref in refs if ref["reported_conflict"].strip().lower() == "none"]
        for address, refs in candidates.items()
        if any(ref["reported_conflict"].strip().lower() == "none" for ref in refs)
    }
    if len(candidates) != 1093:
        raise ValueError(f"Expected 1093 fixpoint candidate addresses, parsed {len(candidates)}")
    candidate_extents: dict[int, tuple[int, int]] = {}
    for address, refs in candidates.items():
        end_candidates = [r["reported_terminator_address"] for r in refs
                          if r.get("reported_terminator_address") is not None]
        if end_candidates:
            candidate_extents[address] = (address, max(end_candidates) + 4)
    candidate_overlap_starts: dict[int, set[int]] = defaultdict(set)
    by_extent = sorted((start, end, candidate) for candidate, (start, end) in candidate_extents.items())
    active: list[tuple[int, int]] = []
    for start, end, candidate in by_extent:
        active = [(other_end, other) for other_end, other in active if other_end > start]
        for _other_end, other in active:
            candidate_overlap_starts[candidate].add(other)
            candidate_overlap_starts[other].add(candidate)
        active.append((end, candidate))
    candidate_rows: list[dict[str, Any]] = []
    proposed: dict[int, int] = dict(accepted)
    counts: Counter[str] = Counter()
    ref_type_counts: Counter[str] = Counter()
    source_class_counts: Counter[str] = Counter()
    overlap_known: list[tuple[int, int, int, int]] = []
    for address, refs in sorted(candidates.items()):
        section = image.section(address, 4)
        exec_section = image.executable_section(address, 4)
        pdata = image.pdata_at(address)
        aligned = address % 4 == 0
        prolog_score, prolog_evidence = image.prolog(address)
        terminator, term_kind = image.terminator(address)
        ctr = image.veneer(address) if exec_section and address % 4 == 0 else None
        code_refs: list[dict[str, Any]] = []
        data_refs: list[dict[str, Any]] = []
        owner_starts: set[int] = set()
        for ref in refs:
            ref_type_counts[ref["type"]] += 1
            source_section = image.section(ref["source"], 4)
            source_exec = image.executable_section(ref["source"], 4) is not None
            owner = image.owner(ref["source"]) if source_exec else None
            source_veneer_target = image.veneer(ref["source"]) if source_exec else None
            source_kind = ("executable code with PDATA owner" if source_exec and owner else
                           "executable code without PDATA owner" if source_exec else
                           "data/non-executable")
            source_class_counts[source_kind] += 1
            evidence = {**ref, "source_section": source_section.get("name") if source_section else None,
                        "source_kind": source_kind,
                        "source_owner": owner[0] if owner else None,
                        "source_veneer_target": source_veneer_target,
                        "source_instruction": image.decode(image.word(ref["source"]))}
            if source_exec:
                code_refs.append(evidence)
                if owner:
                    owner_starts.add(owner[0])
            else:
                data_refs.append(evidence)
        overlap_ranges = []
        inferred_end = candidate_extents.get(address, (address, address))[1]
        independent_code_sites = len({ref["source"] for ref in code_refs})
        exact_veneer_refs = {ref["source"] for ref in code_refs
                             if ref["source_veneer_target"] == address}
        if pdata:
            if address != pdata[0]:
                category, reason = "D", f"inside PDATA {hx(pdata[0])}-{hx(pdata[1])}"
            else:
                category, reason = "A", f"exact PDATA function start, size={pdata[2]}"
                proposed[address] = pdata[2]
        elif not aligned or exec_section is None:
            category = "E"
            reason = "unaligned address" if not aligned else "not wholly inside an executable section"
        else:
            if inferred_end > address:
                # Check the inferred interval against PDATA and all accepted probe functions.
                for start, end, _size in image.pdata_overlaps(address, inferred_end):
                    overlap_ranges.append((start, end, "PDATA"))
                for start, size in accepted.items():
                    if address < start + size and start < inferred_end:
                        overlap_ranges.append((start, start + size, "accepted override"))
                for other in sorted(candidate_overlap_starts.get(address, set())):
                    other_start, other_end = candidate_extents[other]
                    overlap_ranges.append((other_start, other_end, "heuristic candidate"))
            if overlap_ranges:
                category = "E"
                reason = "inferred range overlaps known function extent"
                overlap_known.extend((address, inferred_end, a, b)
                                     for a, b, _kind in overlap_ranges)
            elif ctr is not None:
                ctr_pdata = image.pdata_at(ctr)
                ctr_accepted = ctr in accepted
                independent_owners = len(owner_starts)
                if ctr_pdata and independent_code_sites >= 2:
                    category = "B"
                    reason = (f"exact 16-byte PPC veneer; CTR={hx(ctr)} is PDATA start; "
                              f"{independent_code_sites} independent executable code reference sites")
                    proposed[address] = 16
                else:
                    category = "C"
                    reason = (f"exact PPC veneer to {hx(ctr)}; target boundary="
                              f"{'PDATA' if ctr_pdata else 'unconfirmed'}; "
                              f"independent code referrers={independent_code_sites}")
            elif (independent_code_sites >= 2 and prolog_score >= 3 and terminator is not None
                  and term_kind in {"blr", "bctr"} and terminator >= address + 8):
                category = "B"
                reason = (f"entry prolog evidence ({'; '.join(prolog_evidence)}), terminator "
                          f"{term_kind}@{hx(terminator)}, and {independent_code_sites} independent "
                          "executable code reference sites")
                proposed[address] = terminator + 4 - address
            elif (len(exact_veneer_refs) >= 2 and image.word(address) == 0x4E800020
                  and not image.pdata_overlaps(address, address + 4)):
                category = "B"
                reason = ("single-instruction leaf return blr at entry plus "
                          f"{len(exact_veneer_refs)} exact, independent 16-byte PPC veneers targeting it")
                proposed[address] = 4
            elif not code_refs:
                category = "E"
                reason = "references are data-table/non-executable only; no executable code referrer"
            elif prolog_score or terminator is not None:
                category = "C"
                reason = (f"executable target with partial entry/terminator evidence; "
                          f"prolog score={prolog_score}, terminator={term_kind}@{hx(terminator)}, "
                          f"independent code referrers={len(owner_starts)}")
            else:
                category = "C"
                reason = (f"executable pointer target but no strong boundary pattern; "
                          f"independent code referrers={len(owner_starts)}")
        counts[category] += 1
        candidate_rows.append({
            "address": address, "category": category, "reason": reason,
            "aligned": aligned, "section": section.get("name") if section else None,
            "executable": bool(exec_section),
            "pdata": pdata, "reference_count": len(refs),
            "code_references": code_refs, "data_references": data_refs,
            "owner_count": len(owner_starts), "prolog_score": prolog_score,
            "prolog_evidence": prolog_evidence, "terminator": terminator,
            "terminator_kind": term_kind, "veneer_ctr": ctr,
            "overlaps": overlap_ranges,
            "instructions": image.instructions(address, 8),
        })

    # De-duplicate and sort the complete approved set. Existing probes remain intact.
    override_lines = [
        "# Temporary validated Pure function set; generated by validate_candidates.py.",
        "# Contains accepted batch1/batch2 probes plus only category A/B candidates.",
        "", "[functions]",
    ]
    override_lines.extend(f'"0x{address:08X}" = {{ size = {size} }}'
                          for address, size in sorted(proposed.items()))
    args.output_overrides.parent.mkdir(parents=True, exist_ok=True)
    args.output_overrides.write_text("\n".join(override_lines) + "\n", encoding="utf-8")

    output: list[str] = [
        "Pure pointer-fixpoint candidate validation",
        "==========================================",
        f"Image: {args.image}; map: {args.map}",
        f"Candidates parsed from ledger: {len(candidate_rows)}",
        f"PDATA records: {len(image.pdata)}; sections: {len(image.sections)}; executable sections: {len(image.exec_sections)}",
        f"Reference sites by type: {dict(sorted(ref_type_counts.items()))}",
        f"Reference source classes: {dict(sorted(source_class_counts.items()))}",
        f"Categories: {dict(sorted(counts.items()))}",
        f"Separate rejected range conflicts: {len(conflict_lines)}",
        f"Accepted prior probes + explicit verified veneer + A/B: {len(proposed)} overrides",
        "",
        "Category rules:",
        "  A: exact PDATA start.",
        "  B: non-overlapping boundary evidence plus either (a) strong entry prolog and return with >=2 independent executable code references, (b) exact 16-byte veneer to a PDATA start with >=2 independent executable code references, or (c) a one-word blr leaf with >=2 exact 16-byte PPC veneer references targeting it.",
        "  C: executable pointer target without a sufficiently proven function boundary.",
        "  D: address inside an existing PDATA function.",
        "  E: non-executable/data-only, unaligned, veneer/target conflict, or inferred overlap.",
        "",
        "Rejected range conflicts (candidate extent / source / colliding known extent):",
    ]
    for item in conflict_lines:
        if item["collisions"]:
            collision_text = ", ".join(
                f"{c['kind']} {hx(c['range'][0])}-{hx(c['range'][1])} (overlap at {hx(c['start'])})"
                for c in item["collisions"])
        else:
            collision_text = "collision range not mapped to PDATA or accepted override"
        source_section = image.section(item["source"], 4)
        source_instruction = image.decode(image.word(item["source"]))
        source_interpretation = (
            "literal pointer interpretation is unproven; executable word decodes as an instruction"
            if item["ref_type"] == "literal_be32" and image.executable_section(item["source"]) else
            "reference materialization recorded by analyzer")
        overlap_instruction_rows = []
        for collision in item["collisions"]:
            overlap_instruction_rows.append(
                f"{hx(collision['start'])}: {image.decode(image.word(collision['start']))}")
        output.append(
            f"  {hx(item['start'])}-{hx(item['end'])} from {item['ref_type']} "
            f"at {hx(item['source'])} ({source_section.get('name') if source_section else 'unmapped'}; "
            f"{source_instruction}; {source_interpretation}) => {collision_text}; "
            f"candidate-entry={image.decode(image.word(item['start']))}; "
            f"overlap-instruction={' / '.join(overlap_instruction_rows)}")
    output += ["", "Candidate-by-candidate validation:"]
    for row in candidate_rows:
        pdata = row["pdata"]
        pdata_text = "none" if pdata is None else f"{hx(pdata[0])}-{hx(pdata[1])}"
        output.append(
            f"  {hx(row['address'])} [{row['category']}] section={row['section']} "
            f"exec={row['executable']} aligned={row['aligned']} PDATA={pdata_text} "
            f"refs={row['reference_count']} code_refs={len(row['code_references'])} "
            f"data_refs={len(row['data_references'])} owner_count={row['owner_count']} "
            f"prolog={row['prolog_score']} terminator={row['terminator_kind']}@{hx(row['terminator'])} "
            f"CTR={hx(row['veneer_ctr'])} overlaps={len(row['overlaps'])} reason={row['reason']}")
        for ref in row["code_references"] + row["data_references"]:
            output.append(
                f"    ref {hx(ref['source'])} type={ref['type']} source={ref['source_kind']} "
                f"section={ref['source_section']} owner={hx(ref['source_owner'])} "
                f"instruction={ref['source_instruction']} "
                f"verified_veneer_target={hx(ref['source_veneer_target'])}")
        output.extend(f"    {insn}" for insn in row["instructions"])
    args.output_report.parent.mkdir(parents=True, exist_ok=True)
    args.output_report.write_text("\n".join(output) + "\n", encoding="utf-8")
    print(f"Validated {len(candidate_rows)} candidates: {dict(sorted(counts.items()))}")
    print(f"Rejected range conflicts: {len(conflict_lines)}")
    print(f"Wrote {args.output_report} and {args.output_overrides} ({len(proposed)} functions)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
