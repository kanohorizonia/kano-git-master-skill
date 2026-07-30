#!/usr/bin/env python3
"""Offline Draft 2020-12 conformance gate for KOG audit contracts."""

from __future__ import annotations

import copy
import json
from pathlib import Path
import sys
from typing import Any

from jsonschema import Draft202012Validator
from referencing import Registry, Resource


REPO_ROOT = Path(__file__).resolve().parents[3]
SCHEMA_ROOT = REPO_ROOT / "assets" / "audit" / "schemas"
FIXTURE_ROOT = REPO_ROOT / "assets" / "audit" / "fixtures"


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8") as stream:
        return json.load(stream)


def load_jsonl(path: Path) -> list[Any]:
    with path.open("r", encoding="utf-8", newline="") as stream:
        text = stream.read()
    if not text.endswith("\n") or "\r" in text:
        raise AssertionError(f"{path}: invalid canonical JSONL framing")
    return [json.loads(line) for line in text.splitlines()]


def assert_valid(validator: Draft202012Validator, value: Any, label: str) -> None:
    errors = sorted(validator.iter_errors(value), key=lambda item: list(item.path))
    if errors:
        details = "; ".join(
            f"{'/'.join(map(str, error.path)) or '$'}: {error.message}"
            for error in errors[:5]
        )
        raise AssertionError(f"{label}: expected schema-valid value: {details}")


def assert_invalid(validator: Draft202012Validator, value: Any, label: str) -> None:
    if not next(validator.iter_errors(value), None):
        raise AssertionError(f"{label}: expected schema rejection")


def main() -> int:
    event_schema = load_json(SCHEMA_ROOT / "kog.auditEvent.v1.schema.json")
    receipt_schema = load_json(SCHEMA_ROOT / "kog.runReceipt.v1.schema.json")
    Draft202012Validator.check_schema(event_schema)
    Draft202012Validator.check_schema(receipt_schema)

    registry = (
        Registry()
        .with_resource(event_schema["$id"], Resource.from_contents(event_schema))
        .with_resource(receipt_schema["$id"], Resource.from_contents(receipt_schema))
    )
    event_validator = Draft202012Validator(event_schema, registry=registry)
    receipt_validator = Draft202012Validator(receipt_schema, registry=registry)

    golden_events = load_jsonl(
        FIXTURE_ROOT / "golden" / "audit-events.v1.jsonl"
    )
    golden_receipt = load_json(
        FIXTURE_ROOT / "golden" / "run-receipt.v1.json"
    )
    for index, event in enumerate(golden_events):
        assert_valid(event_validator, event, f"golden event {index}")
    assert_valid(receipt_validator, golden_receipt, "golden receipt")

    malformed_hash = copy.deepcopy(golden_events[0])
    malformed_hash["planSha256"] = "ABC123"
    assert_invalid(event_validator, malformed_hash, "malformed plan hash")

    raw_command = copy.deepcopy(golden_events[0])
    raw_command["command"] = "git push origin main"
    assert_invalid(event_validator, raw_command, "forbidden raw command")

    contradictory_success = copy.deepcopy(golden_events[0])
    contradictory_success["outcome"] = {
        "status": "succeeded",
        "exitCode": 7,
        "reasonCode": "REMOTE_PUSH_FAILED",
        "retryable": True,
    }
    assert_invalid(
        event_validator,
        contradictory_success,
        "contradictory success",
    )

    missing_terminal = copy.deepcopy(golden_receipt)
    del missing_terminal["terminalOutcome"]
    assert_invalid(
        receipt_validator,
        missing_terminal,
        "missing terminal outcome",
    )

    too_large_attempt = copy.deepcopy(golden_events[0])
    too_large_attempt["attempt"] = 4_294_967_296
    assert_invalid(event_validator, too_large_attempt, "uint32 attempt overflow")

    for repository_id in (
        "/absolute",
        "..",
        "repos\\private",
        "repos//nested",
        "repos/trailing/",
    ):
        invalid_repository = copy.deepcopy(golden_events[0])
        invalid_repository["repository"]["id"] = repository_id
        assert_invalid(
            event_validator,
            invalid_repository,
            f"logical repository id {repository_id!r}",
        )

    newline_mutations = (
        ("event id", ("eventId",), "event-001\n"),
        ("repository id", ("repository", "id"), "repos/kog\n"),
        ("branch", ("repository", "before", "branch"), "main\n"),
        ("phase", ("phase",), "mutation\n"),
        (
            "artifact content type",
            ("artifacts", 0, "contentType"),
            "text/x-diff\n",
        ),
        ("plan hash", ("planSha256",), "c" * 64 + "\n"),
        (
            "UTC timestamp",
            ("startedAtUtc",),
            "2026-07-30T08:00:00Z\n",
        ),
    )
    for label, path, value in newline_mutations:
        invalid_newline = copy.deepcopy(golden_events[0])
        target = invalid_newline
        for component in path[:-1]:
            target = target[component]
        target[path[-1]] = value
        assert_invalid(
            event_validator,
            invalid_newline,
            f"newline-terminated {label}",
        )

    unknown_root = copy.deepcopy(golden_events[0])
    unknown_root["producerMetadata"] = {"secret": "must-not-pass"}
    assert_invalid(event_validator, unknown_root, "closed event root")

    zero_event = copy.deepcopy(golden_receipt)
    zero_event["firstSequence"] = 0
    zero_event["lastSequence"] = 0
    zero_event["eventCount"] = 0
    zero_event["eventStreamSha256"] = (
        "e3b0c44298fc1c149afbf4c8996fb924"
        "27ae41e4649b934ca495991b7852b855"
    )
    zero_event["terminalOutcome"] = {
        "status": "unknown",
        "exitCode": None,
        "reasonCode": "CRASH_BEFORE_FIRST_EVENT",
        "retryable": True,
    }
    zero_event["repositories"] = []
    assert_valid(receipt_validator, zero_event, "zero-event crash receipt")

    wrong_empty_hash = copy.deepcopy(zero_event)
    wrong_empty_hash["eventStreamSha256"] = "a" * 64
    assert_invalid(
        receipt_validator,
        wrong_empty_hash,
        "zero-event non-empty-stream hash",
    )

    fabricated_zero_event = copy.deepcopy(zero_event)
    fabricated_zero_event["repositories"] = golden_receipt["repositories"]
    assert_invalid(
        receipt_validator,
        fabricated_zero_event,
        "zero-event fabricated repository",
    )

    print(
        "audit-schema-test: Draft 2020-12 schemas, offline refs, "
        "goldens, and parity mutations passed"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:  # noqa: BLE001 - release gate must emit one failure
        print(f"audit-schema-test: FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
