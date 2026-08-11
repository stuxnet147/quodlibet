#!/usr/bin/env python3
"""Measure relational loop-proof coverage on corpus self-pairs.

The input is a ``quodlibet coverage`` detail TSV. Only rows whose third field
is ``lowered`` are judged, and the function name comes from that row rather
than assuming the corpus target is always named ``FUN_0``.

The canonical WSL validation invocation uses one worker for uncontended latency:

    PYTHONPATH=out/build/linux-clang/bindings/python/package \
      <cmake-python> scripts/perf/loop-proof.py \
        out/corpus/val/detail-g9.tsv \
        --jsonl out/loop-proof/val.jsonl \
        --aggregate out/loop-proof/val.json \
        --workers 1 --timeout-ms 20000

Pass a previous JSONL file with ``--before-jsonl`` to report how many formerly
UNKNOWN loop samples became proved. The harness performs no unrolling and does
not infer metrics from diagnostic text. It consumes the structured
``CheckResult.loop_proof`` record produced by the proof method.

``--legacy-baseline`` permits running the immediately preceding package, which
does not expose loop telemetry. Its JSONL is intended only as ``--before-jsonl``
input for a telemetry-capable after run; loop classification then comes from
the after record with the same sample id.
"""

from __future__ import annotations

import argparse
import collections
import csv
import dataclasses
import datetime as dt
import json
import math
import os
import pathlib
import subprocess
import sys
from collections.abc import Mapping, Sequence
from typing import Any


OBSERVATIONS = ("return-value", "memory", "termination", "traps")
EXPECTED_LOOP_FIELDS = {
    "applicable",
    "cyclic",
    "natural_loop_count",
    "noncanonical_cycle",
    "left_loop_count",
    "right_loop_count",
    "paired_loop_count",
    "all_loops_paired",
    "invariant_generated_count",
    "induction_proved_count",
    "summary_attempted_count",
    "summary_proved_count",
    "reflexivity_proved_count",
    "fallback_reached",
    "fallback_attempted",
    "concrete_domain_witness",
    "strategy",
    "failure_reason",
    "stage_ns",
    "stage_reached",
}


def posix_path(path: pathlib.Path | None) -> str | None:
    return None if path is None else path.resolve().as_posix()


def atomic_write_text(path: pathlib.Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp-{os.getpid()}")
    try:
        temporary.write_text(text, encoding="utf-8", newline="\n")
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def json_ready(value: Any) -> Any:
    if dataclasses.is_dataclass(value) and not isinstance(value, type):
        return json_ready(dataclasses.asdict(value))
    if isinstance(value, Mapping):
        return {str(key): json_ready(item) for key, item in value.items()}
    if isinstance(value, (list, tuple)):
        return [json_ready(item) for item in value]
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    return str(value)


def require_nonnegative_int(raw: Mapping[str, Any], name: str) -> int:
    value = raw[name]
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"loop_proof.{name} must be a non-negative integer")
    return value


def require_bool(raw: Mapping[str, Any], name: str) -> bool:
    value = raw[name]
    if not isinstance(value, bool):
        raise ValueError(f"loop_proof.{name} must be a boolean")
    return value


def require_string(raw: Mapping[str, Any], name: str) -> str:
    value = raw[name]
    if not isinstance(value, str):
        raise ValueError(f"loop_proof.{name} must be a string")
    return value


def normalize_loop_proof(value: Any) -> tuple[dict[str, Any] | None, list[str]]:
    if value is None:
        return None, []
    converted = json_ready(value)
    if not isinstance(converted, Mapping):
        raise ValueError("CheckResult.loop_proof must be a dataclass or mapping")
    raw = dict(converted)
    missing = sorted(EXPECTED_LOOP_FIELDS - raw.keys())
    if missing:
        raise ValueError(
            "CheckResult.loop_proof is missing fields: " + ", ".join(missing)
        )

    normalized = {
        "applicable": require_bool(raw, "applicable"),
        "cyclic": require_bool(raw, "cyclic"),
        "natural_loop_count": require_nonnegative_int(
            raw, "natural_loop_count"
        ),
        "noncanonical_cycle": require_bool(raw, "noncanonical_cycle"),
        "left_loop_count": require_nonnegative_int(raw, "left_loop_count"),
        "right_loop_count": require_nonnegative_int(raw, "right_loop_count"),
        "paired_loop_count": require_nonnegative_int(raw, "paired_loop_count"),
        "all_loops_paired": require_bool(raw, "all_loops_paired"),
        "invariant_generated_count": require_nonnegative_int(
            raw, "invariant_generated_count"
        ),
        "induction_proved_count": require_nonnegative_int(
            raw, "induction_proved_count"
        ),
        "summary_attempted_count": require_nonnegative_int(
            raw, "summary_attempted_count"
        ),
        "summary_proved_count": require_nonnegative_int(
            raw, "summary_proved_count"
        ),
        "reflexivity_proved_count": require_nonnegative_int(
            raw, "reflexivity_proved_count"
        ),
        "fallback_reached": require_bool(raw, "fallback_reached"),
        "fallback_attempted": require_bool(raw, "fallback_attempted"),
        "concrete_domain_witness": require_bool(
            raw, "concrete_domain_witness"
        ),
        "strategy": require_string(raw, "strategy"),
        "failure_reason": require_string(raw, "failure_reason"),
    }

    proof_eligible = raw.get("proof_eligible")
    if "proof_eligible" in raw and not isinstance(proof_eligible, bool):
        raise ValueError("loop_proof.proof_eligible must be a boolean")
    normalized["proof_eligible"] = proof_eligible
    for name in ("induction_answer", "summary_answer", "reflexivity_answer"):
        answer = raw.get(name)
        if name in raw and not isinstance(answer, str):
            raise ValueError(f"loop_proof.{name} must be a string")
        normalized[name] = answer

    digests = raw.get("digests")
    if "digests" in raw:
        if not isinstance(digests, Mapping):
            raise ValueError("loop_proof.digests must be a mapping")
        for name, digest in digests.items():
            if not isinstance(name, str) or not isinstance(digest, str):
                raise ValueError(
                    "loop_proof.digests must map strings to strings"
                )
        normalized["digests"] = dict(digests)
    else:
        normalized["digests"] = None

    stage_ns = raw["stage_ns"]
    stage_reached = raw["stage_reached"]
    if not isinstance(stage_ns, Mapping) or not isinstance(stage_reached, Mapping):
        raise ValueError(
            "loop_proof.stage_ns and loop_proof.stage_reached must be mappings"
        )
    stage_names = sorted(set(stage_ns) | set(stage_reached))
    stages: dict[str, dict[str, Any]] = {}
    for name_value in stage_names:
        name = str(name_value)
        reached = stage_reached.get(name_value, False)
        elapsed = stage_ns.get(name_value, 0)
        if not isinstance(reached, bool):
            raise ValueError(f"loop_proof.stage_reached[{name!r}] must be boolean")
        if isinstance(elapsed, bool) or not isinstance(elapsed, int) or elapsed < 0:
            raise ValueError(
                f"loop_proof.stage_ns[{name!r}] must be a non-negative integer"
            )
        if not reached and elapsed != 0:
            raise ValueError(
                f"loop_proof stage {name!r} has latency but was not reached"
            )
        measured = reached and not (name == "fallback" and elapsed == 0)
        stages[name] = {
            "reached": reached,
            "measured": measured,
            "elapsed_ns": elapsed if measured else None,
        }
    normalized["stages"] = stages

    warnings: list[str] = []
    pairing_opportunities = max(
        normalized["left_loop_count"], normalized["right_loop_count"]
    )
    if normalized["paired_loop_count"] > pairing_opportunities:
        warnings.append("paired_loop_count exceeds pairing opportunities")
    if (
        normalized["invariant_generated_count"]
        > normalized["paired_loop_count"]
    ):
        warnings.append("invariant_generated_count exceeds paired_loop_count")
    if (
        normalized["induction_proved_count"]
        > normalized["invariant_generated_count"]
    ):
        warnings.append(
            "induction_proved_count exceeds invariant_generated_count"
        )
    if (
        normalized["summary_proved_count"]
        > normalized["summary_attempted_count"]
    ):
        warnings.append("summary_proved_count exceeds summary_attempted_count")
    if normalized["reflexivity_proved_count"] > normalized["paired_loop_count"]:
        warnings.append("reflexivity_proved_count exceeds paired_loop_count")
    return normalized, warnings


def host_path(path: pathlib.Path) -> pathlib.Path:
    """Translate a corpus path recorded on Windows when running under WSL."""
    text = str(path)
    if os.name != "nt" and len(text) >= 3 and text[1:3] in (":/", ":\\"):
        drive = text[0].lower()
        rest = text[3:].replace("\\", "/")
        return pathlib.Path(f"/mnt/{drive}/{rest}")
    return path


def source_has_loop_syntax(source: str) -> bool:
    """Conservative lexical loop marker used only for the error denominator."""
    code = []
    index = 0
    while index < len(source):
        if source.startswith("//", index):
            end = source.find("\n", index + 2)
            index = len(source) if end < 0 else end
        elif source.startswith("/*", index):
            end = source.find("*/", index + 2)
            index = len(source) if end < 0 else end + 2
        elif source[index] in ('"', "'"):
            quote = source[index]
            code.append(" ")
            index += 1
            while index < len(source):
                if source[index] == "\\":
                    index += 2
                elif source[index] == quote:
                    index += 1
                    break
                else:
                    index += 1
        else:
            code.append(source[index])
            index += 1
    tokens = "".join(code)
    import re

    return re.search(r"\b(?:for|while|do)\b", tokens) is not None


def load_manifest(path: pathlib.Path) -> tuple[dict[str, Any], dict[str, dict]]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict) or not isinstance(document.get("units"), list):
        raise ValueError(f"{path}: manifest must contain a units array")
    by_path: dict[str, dict] = {}
    for unit in document["units"]:
        if not isinstance(unit, dict) or not isinstance(unit.get("file"), str):
            raise ValueError(f"{path}: malformed manifest unit")
        recorded = host_path(pathlib.Path(unit["file"]))
        unit_path = (
            recorded.resolve()
            if recorded.is_absolute()
            else (path.parent / recorded).resolve()
        )
        key = os.path.normcase(str(unit_path))
        if key in by_path:
            raise ValueError(f"{path}: duplicate unit path {unit_path}")
        by_path[key] = unit
    metadata = {key: value for key, value in document.items() if key != "units"}
    metadata["unit_count"] = len(document["units"])
    return metadata, by_path


def load_detail(
    path: pathlib.Path,
    manifest_units: Mapping[str, dict],
) -> list[dict[str, Any]]:
    samples: list[dict[str, Any]] = []
    seen: set[tuple[str, str]] = set()
    with path.open(newline="", encoding="utf-8") as handle:
        for line_number, row in enumerate(csv.reader(handle, delimiter="\t"), 1):
            if len(row) < 3 or row[2] != "lowered":
                continue
            source_path = host_path(pathlib.Path(row[0]))
            if not source_path.is_absolute():
                from_cwd = source_path.resolve()
                from_detail = (path.parent / source_path).resolve()
                source_path = from_cwd if from_cwd.exists() else from_detail
            else:
                source_path = source_path.resolve()
            function = row[1]
            if not function:
                raise ValueError(f"{path}:{line_number}: empty function name")
            key = (os.path.normcase(str(source_path)), function)
            if key in seen:
                raise ValueError(
                    f"{path}:{line_number}: duplicate sample {source_path}:{function}"
                )
            seen.add(key)
            manifest_entry = manifest_units.get(key[0])
            if manifest_entry is None:
                raise ValueError(
                    f"{path}:{line_number}: {source_path} is absent from manifest"
                )
            if not source_path.is_file():
                raise ValueError(f"{path}:{line_number}: missing {source_path}")
            digest = manifest_entry.get("digest")
            if not isinstance(digest, str) or not digest:
                raise ValueError(
                    f"{path}:{line_number}: manifest unit has no digest"
                )
            samples.append(
                {
                    "sample_id": f"{digest}:{function}",
                    "unit_digest": digest,
                    "unit": source_path.as_posix(),
                    "function": function,
                    "manifest_records": manifest_entry.get("records"),
                    "manifest_project": manifest_entry.get("project"),
                    "manifest_subject": manifest_entry.get("subject"),
                }
            )
    return samples


def rate(numerator: int, denominator: int) -> dict[str, Any]:
    return {
        "numerator": numerator,
        "denominator": denominator,
        "rate": None if denominator == 0 else numerator / denominator,
    }


def nearest_rank(values: Sequence[int], quantile: float) -> int | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(quantile * len(ordered)) - 1)
    return ordered[index]


def stage_summary(records: Sequence[Mapping[str, Any]]) -> dict[str, dict]:
    values: dict[str, list[int]] = collections.defaultdict(list)
    stage_names: set[str] = set()
    for record in records:
        loop_proof = record.get("loop_proof")
        if (
            not isinstance(loop_proof, Mapping)
            or not loop_proof.get("applicable")
        ):
            continue
        stages = loop_proof.get("stages")
        if not isinstance(stages, Mapping):
            continue
        for name, stage in stages.items():
            stage_names.add(str(name))
            if isinstance(stage, Mapping) and stage.get("measured") is True:
                elapsed = stage.get("elapsed_ns")
                if isinstance(elapsed, int) and not isinstance(elapsed, bool):
                    values[str(name)].append(elapsed)
    summary = {}
    for name in sorted(stage_names):
        samples = values[name]
        p50_ns = nearest_rank(samples, 0.50)
        p95_ns = nearest_rank(samples, 0.95)
        summary[name] = {
            "reached": sum(
                1
                for record in records
                if isinstance(record.get("loop_proof"), Mapping)
                and record["loop_proof"].get("applicable")
                and isinstance(record["loop_proof"].get("stages"), Mapping)
                and isinstance(
                    record["loop_proof"]["stages"].get(name), Mapping
                )
                and record["loop_proof"]["stages"][name].get("reached")
                is True
            ),
            "measured": len(samples),
            "p50_ns": p50_ns,
            "p95_ns": p95_ns,
            "p50_ms": None if p50_ns is None else p50_ns / 1_000_000,
            "p95_ms": None if p95_ns is None else p95_ns / 1_000_000,
        }
    return summary


def aggregate_records(records: Sequence[Mapping[str, Any]]) -> dict[str, Any]:
    verdicts: collections.Counter[str] = collections.Counter()
    statuses: collections.Counter[str] = collections.Counter()
    strategies: collections.Counter[str] = collections.Counter()
    failure_reasons: collections.Counter[str] = collections.Counter()
    induction_answers: collections.Counter[str] = collections.Counter()
    summary_answers: collections.Counter[str] = collections.Counter()
    reflexivity_answers: collections.Counter[str] = collections.Counter()
    errors = 0
    loop_records: list[Mapping[str, Any]] = []
    cyclic_samples = 0
    natural_samples = 0
    noncanonical_samples = 0
    all_loops_paired_samples = 0
    proof_eligible_samples = 0
    proof_eligible_reported = 0
    fallback_reached_samples = 0
    fallback_attempted_samples = 0
    final_unknown_loop = 0
    pairing_opportunities = 0
    paired_loops = 0
    generated_invariants = 0
    proved_inductions = 0
    attempted_summaries = 0
    proved_summaries = 0
    proved_reflexivities = 0
    concrete_domain_witnesses = 0
    left_loops = 0
    right_loops = 0
    natural_loops = 0
    warnings = 0
    loop_error_samples = 0

    for record in records:
        verdict = record.get("verdict")
        if isinstance(verdict, str):
            verdicts[verdict] += 1
        else:
            errors += 1
            loop_error_samples += int(bool(record.get("source_loop_syntax")))
        status = record.get("status")
        if isinstance(status, Mapping) and isinstance(status.get("kind"), str):
            statuses[status["kind"]] += 1
        warnings += len(record.get("metric_warnings", []))
        loop_proof = record.get("loop_proof")
        if not isinstance(loop_proof, Mapping) or not loop_proof.get("applicable"):
            continue
        loop_records.append(record)
        cyclic_samples += int(loop_proof["cyclic"])
        natural_samples += int(loop_proof["natural_loop_count"] > 0)
        noncanonical_samples += int(loop_proof["noncanonical_cycle"])
        if loop_proof["natural_loop_count"] > 0:
            all_loops_paired_samples += int(loop_proof["all_loops_paired"])
        if loop_proof.get("proof_eligible") is not None:
            proof_eligible_reported += 1
            proof_eligible_samples += int(loop_proof["proof_eligible"])
        fallback_reached_samples += int(loop_proof["fallback_reached"])
        fallback_attempted_samples += int(loop_proof["fallback_attempted"])
        final_unknown_loop += int(verdict == "unknown")
        left = int(loop_proof["left_loop_count"])
        right = int(loop_proof["right_loop_count"])
        left_loops += left
        right_loops += right
        natural_loops += int(loop_proof["natural_loop_count"])
        pairing_opportunities += max(left, right)
        paired_loops += int(loop_proof["paired_loop_count"])
        generated_invariants += int(loop_proof["invariant_generated_count"])
        proved_inductions += int(loop_proof["induction_proved_count"])
        attempted_summaries += int(loop_proof["summary_attempted_count"])
        proved_summaries += int(loop_proof["summary_proved_count"])
        proved_reflexivities += int(loop_proof["reflexivity_proved_count"])
        concrete_domain_witnesses += int(loop_proof["concrete_domain_witness"])
        strategies[str(loop_proof["strategy"])] += 1
        induction_answer = loop_proof.get("induction_answer")
        if isinstance(induction_answer, str):
            induction_answers[induction_answer] += 1
        summary_answer = loop_proof.get("summary_answer")
        if isinstance(summary_answer, str):
            summary_answers[summary_answer] += 1
        reflexivity_answer = loop_proof.get("reflexivity_answer")
        if isinstance(reflexivity_answer, str):
            reflexivity_answers[reflexivity_answer] += 1
        reason = str(loop_proof["failure_reason"])
        if reason:
            failure_reasons[reason] += 1

    explicit_results = len(records) - errors
    final_unknown_all = verdicts["unknown"]
    loop_population_with_errors = len(loop_records) + loop_error_samples
    return {
        "raw_counts": {
            "probed_samples": len(records),
            "explicit_results": explicit_results,
            "errors": errors,
            "source_loop_error_samples": loop_error_samples,
            "loop_population_with_errors": loop_population_with_errors,
            "applicable_loop_samples": len(loop_records),
            "cyclic_samples": cyclic_samples,
            "natural_loop_samples": natural_samples,
            "noncanonical_cycle_samples": noncanonical_samples,
            "all_loops_paired_samples": all_loops_paired_samples,
            "proof_eligible_samples": proof_eligible_samples,
            "proof_eligible_reported_samples": proof_eligible_reported,
            "left_loops": left_loops,
            "right_loops": right_loops,
            "natural_loops": natural_loops,
            "pairing_opportunities": pairing_opportunities,
            "paired_loops": paired_loops,
            "invariants_generated": generated_invariants,
            "inductions_proved": proved_inductions,
            "summaries_attempted": attempted_summaries,
            "summaries_proved": proved_summaries,
            "reflexivities_proved": proved_reflexivities,
            "concrete_domain_witness_samples": concrete_domain_witnesses,
            "fallback_reached_samples": fallback_reached_samples,
            "fallback_attempted_samples": fallback_attempted_samples,
            "final_unknown_loop_samples": final_unknown_loop,
            "final_unknown_all_samples": final_unknown_all,
            "metric_warning_count": warnings,
        },
        "rates": {
            "cyclic_samples_of_all": rate(cyclic_samples, len(records)),
            "cyclic_samples_of_applicable": rate(
                cyclic_samples, len(loop_records)
            ),
            "natural_loop_samples_of_all": rate(natural_samples, len(records)),
            "natural_samples_of_applicable": rate(
                natural_samples, len(loop_records)
            ),
            "noncanonical_cycles_of_applicable": rate(
                noncanonical_samples, len(loop_records)
            ),
            "noncanonical_cycles_of_cyclic": rate(
                noncanonical_samples, cyclic_samples
            ),
            "all_loops_paired_samples": rate(
                all_loops_paired_samples, natural_samples
            ),
            "loop_pairing_success": rate(paired_loops, pairing_opportunities),
            "proof_eligible_of_reported_loop_samples": rate(
                proof_eligible_samples, proof_eligible_reported
            ),
            "structural_invariant_generation_success": rate(
                generated_invariants, paired_loops
            ),
            "smt_induction_proof_success": rate(
                proved_inductions, generated_invariants
            ),
            "affine_summary_success": rate(
                proved_summaries, attempted_summaries
            ),
            "fallback_reached_of_loop_samples": rate(
                fallback_reached_samples, len(loop_records)
            ),
            "fallback_attempted_of_loop_samples": rate(
                fallback_attempted_samples, len(loop_records)
            ),
            "final_unknown_of_loop_samples": rate(
                final_unknown_loop, len(loop_records)
            ),
            "final_unknown_or_error_of_loop_population": rate(
                final_unknown_loop + loop_error_samples,
                loop_population_with_errors,
            ),
            "source_loop_errors_of_loop_population": rate(
                loop_error_samples, loop_population_with_errors
            ),
            "exact_reflexivity_success": rate(
                proved_reflexivities, paired_loops
            ),
            "final_unknown_of_all_probes": rate(
                final_unknown_all, len(records)
            ),
            "final_unknown_of_explicit_results": rate(
                final_unknown_all, explicit_results
            ),
        },
        "verdicts": dict(sorted(verdicts.items())),
        "statuses": dict(sorted(statuses.items())),
        "strategies": dict(sorted(strategies.items())),
        "induction_answers": dict(sorted(induction_answers.items())),
        "summary_answers": dict(sorted(summary_answers.items())),
        "reflexivity_answers": dict(sorted(reflexivity_answers.items())),
        "failure_reasons": dict(sorted(failure_reasons.items())),
        "stage_latency_reached_only": stage_summary(records),
        "percentile_method": "nearest-rank",
    }


def load_jsonl(path: pathlib.Path) -> list[dict[str, Any]]:
    records = []
    seen: set[str] = set()
    with path.open(encoding="utf-8") as handle:
        for line_number, line in enumerate(handle, 1):
            if not line.strip():
                continue
            value = json.loads(line)
            if not isinstance(value, dict) or not isinstance(
                value.get("sample_id"), str
            ):
                raise ValueError(f"{path}:{line_number}: invalid sample record")
            if value["sample_id"] in seen:
                raise ValueError(
                    f"{path}:{line_number}: duplicate sample_id "
                    f"{value['sample_id']}"
                )
            seen.add(value["sample_id"])
            records.append(value)
    return records


def is_loop_record(record: Mapping[str, Any]) -> bool:
    loop_proof = record.get("loop_proof")
    return bool(
        isinstance(loop_proof, Mapping)
        and loop_proof.get("applicable")
        and (
            loop_proof.get("cyclic")
            or int(loop_proof.get("natural_loop_count", 0)) > 0
        )
    )


def compare_before(
    before: Sequence[Mapping[str, Any]],
    after: Sequence[Mapping[str, Any]],
) -> dict[str, Any]:
    before_by_id = {record["sample_id"]: record for record in before}
    after_by_id = {record["sample_id"]: record for record in after}
    common = sorted(set(before_by_id) & set(after_by_id))
    baseline_unknown = 0
    recovered = 0
    remaining_unknown = 0
    other_after = 0
    regressions = 0
    for sample_id in common:
        old = before_by_id[sample_id]
        new = after_by_id[sample_id]
        loop_sample = is_loop_record(old) or is_loop_record(new)
        if loop_sample and old.get("verdict") == "unknown":
            baseline_unknown += 1
            verdict = new.get("verdict")
            if isinstance(verdict, str) and verdict.startswith("proved-"):
                recovered += 1
            elif verdict == "unknown":
                remaining_unknown += 1
            else:
                other_after += 1
        if (
            loop_sample
            and isinstance(old.get("verdict"), str)
            and str(old["verdict"]).startswith("proved-")
            and new.get("verdict") == "unknown"
        ):
            regressions += 1
    return {
        "before_samples": len(before),
        "after_samples": len(after),
        "common_samples": len(common),
        "missing_from_after": len(set(before_by_id) - set(after_by_id)),
        "new_in_after": len(set(after_by_id) - set(before_by_id)),
        "baseline_loop_unknown": baseline_unknown,
        "recovered_to_proof": recovered,
        "remaining_unknown": remaining_unknown,
        "other_after_outcome": other_after,
        "regressed_proof_to_unknown": regressions,
        "recovery_rate": rate(recovered, baseline_unknown),
    }


def git_context(root: pathlib.Path) -> dict[str, Any]:
    def run(*arguments: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            ["git", *arguments],
            cwd=root,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            check=False,
        )

    revision = run("rev-parse", "HEAD")
    status = run("status", "--porcelain")
    return {
        "commit": revision.stdout.strip() if revision.returncode == 0 else None,
        "dirty": status.returncode != 0 or bool(status.stdout.strip()),
    }


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("detail", type=pathlib.Path)
    parser.add_argument("--manifest", type=pathlib.Path)
    parser.add_argument("--jsonl", required=True, type=pathlib.Path)
    parser.add_argument("--aggregate", required=True, type=pathlib.Path)
    parser.add_argument("--before-jsonl", type=pathlib.Path)
    parser.add_argument(
        "--legacy-baseline",
        action="store_true",
        help="allow a package that predates CheckResult.loop_proof",
    )
    parser.add_argument("--workers", type=int, default=1)
    parser.add_argument("--timeout-ms", type=int, default=20000)
    parser.add_argument(
        "--expected-samples",
        type=int,
        default=1044,
        help="refuse denominator drift; pass 0 to disable (default: 1044)",
    )
    args = parser.parse_args(argv)
    if args.workers < 0:
        parser.error("--workers must be zero or positive")
    if args.timeout_ms <= 0:
        parser.error("--timeout-ms must be positive")
    if args.expected_samples < 0:
        parser.error("--expected-samples must be zero or positive")
    return args


def main(argv: Sequence[str]) -> int:
    args = parse_args(argv)
    detail = args.detail.resolve()
    manifest = (
        args.manifest.resolve()
        if args.manifest is not None
        else (detail.parent / "manifest.json").resolve()
    )
    output_jsonl = args.jsonl.resolve()
    output_aggregate = args.aggregate.resolve()
    before_jsonl = (
        None if args.before_jsonl is None else args.before_jsonl.resolve()
    )
    before_records = (
        None if before_jsonl is None else load_jsonl(before_jsonl)
    )
    inputs = {detail, manifest}
    if before_jsonl is not None:
        inputs.add(before_jsonl)
    if output_jsonl in inputs or output_aggregate in inputs:
        raise ValueError("output paths must not overwrite an input")
    if output_jsonl == output_aggregate:
        raise ValueError("--jsonl and --aggregate must be different paths")

    manifest_metadata, manifest_units = load_manifest(manifest)
    samples = load_detail(detail, manifest_units)
    if args.expected_samples and len(samples) != args.expected_samples:
        raise ValueError(
            f"expected {args.expected_samples} lowered samples, found {len(samples)}"
        )

    import quodlibet  # noqa: PLC0415

    fields = getattr(quodlibet.CheckResult, "__dataclass_fields__", {})
    has_loop_telemetry = "loop_proof" in fields
    if not has_loop_telemetry and not args.legacy_baseline:
        raise RuntimeError(
            "this quodlibet Python package does not expose CheckResult.loop_proof"
        )
    if has_loop_telemetry and args.legacy_baseline:
        raise RuntimeError(
            "--legacy-baseline was requested, but this package already exposes "
            "CheckResult.loop_proof"
        )
    backend = quodlibet.backend_info()
    if not backend.get("available"):
        raise RuntimeError("the configured Bitwuzla backend is unavailable")

    specs = []
    for sample in samples:
        source = pathlib.Path(sample["unit"]).read_text(encoding="utf-8")
        sample["source_loop_syntax"] = source_has_loop_syntax(source)
        specs.append(
            quodlibet.CheckSpec(
                left_source=source,
                left_function=sample["function"],
                right_source=source,
                right_function=sample["function"],
                relation="equivalence",
                ub_policy="must-match",
                observations=OBSERVATIONS,
                trust_smt_backend=True,
                solver_timeout_ms=args.timeout_ms,
            )
        )

    started = dt.datetime.now(dt.timezone.utc)
    print(
        f"loop-proof: judging {len(specs)} self-pairs with "
        f"workers={args.workers}, timeout_ms={args.timeout_ms}",
        flush=True,
    )
    results = quodlibet.check_batch(specs, workers=args.workers)
    finished = dt.datetime.now(dt.timezone.utc)

    records: list[dict[str, Any]] = []
    for sample, result in zip(samples, results, strict=True):
        record = dict(sample)
        if isinstance(result, Exception):
            record.update(
                {
                    "verdict": None,
                    "status": None,
                    "diagnostic": str(result),
                    "error_type": type(result).__name__,
                    "usage": None,
                    "evidence": None,
                    "loop_proof": None,
                    "metric_warnings": [],
                }
            )
        else:
            if has_loop_telemetry:
                loop_proof, metric_warnings = normalize_loop_proof(
                    result.loop_proof
                )
            else:
                loop_proof, metric_warnings = None, []
            record.update(
                {
                    "verdict": result.verdict,
                    "status": {
                        "kind": result.status.kind,
                        "message": result.status.message,
                    },
                    "diagnostic": result.diagnostic,
                    "error_type": None,
                    "usage": json_ready(result.evidence.usage),
                    "evidence": json_ready(result.evidence),
                    "loop_proof": loop_proof,
                    "metric_warnings": metric_warnings,
                }
            )
        records.append(record)

    jsonl_text = "".join(
        json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n"
        for record in records
    )
    atomic_write_text(output_jsonl, jsonl_text)

    root = pathlib.Path(__file__).resolve().parents[2]
    aggregate = {
        "schema_version": 1,
        "run": {
            "started_utc": started.isoformat(),
            "finished_utc": finished.isoformat(),
            "wall_seconds": (finished - started).total_seconds(),
            "detail": posix_path(detail),
            "manifest": posix_path(manifest),
            "jsonl": posix_path(output_jsonl),
            "aggregate": posix_path(output_aggregate),
            "before_jsonl": posix_path(before_jsonl),
            "options": {
                "self_pair": True,
                "relation": "equivalence",
                "ub_policy": "must-match",
                "observations": list(OBSERVATIONS),
                "trust_smt_backend": True,
                "solver_timeout_ms": args.timeout_ms,
                "workers": args.workers,
                "expected_samples": args.expected_samples,
                "bounded_unrolling": False,
                "loop_telemetry_available": has_loop_telemetry,
            },
            "manifest_metadata": manifest_metadata,
            "sample_ids": [sample["sample_id"] for sample in samples],
            "python_executable": pathlib.Path(sys.executable).resolve().as_posix(),
            "python_version": sys.version,
            "core_version": quodlibet.core_version(),
            "backend": json_ready(backend),
            "git": git_context(root),
            "latency_note": (
                "serial, uncontended"
                if args.workers == 1
                else "parallel worker contention is included"
            ),
        },
        "metrics": aggregate_records(records),
    }
    if before_records is not None:
        aggregate["recovery"] = compare_before(before_records, records)
    atomic_write_text(
        output_aggregate,
        json.dumps(aggregate, indent=2, ensure_ascii=False, sort_keys=True) + "\n",
    )

    counts = aggregate["metrics"]["raw_counts"]
    print(
        "loop-proof: "
        f"loop_samples={counts['applicable_loop_samples']} "
        f"proved_inductions={counts['inductions_proved']} "
        f"proved_summaries={counts['summaries_proved']} "
        f"final_unknown={counts['final_unknown_loop_samples']}",
        flush=True,
    )
    print(f"loop-proof: wrote {output_jsonl} and {output_aggregate}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
