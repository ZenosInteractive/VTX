import json
import subprocess
import sys

# ==========================================
# CONFIGURATION
# ==========================================
VTX_CLI_PATH = "../../../build/bin/Release/vtx_cli.exe"
TEST_FILE = "../../../samples/content/reader/rl/rl_proto.vtx"


def read_full_response(process):
    accumulated = ""
    while True:
        line = process.stdout.readline()
        if not line:
            return None

        clean_text = line.strip()
        if accumulated == "" and not clean_text.startswith("{") and not clean_text.startswith("["):
            if clean_text:
                print(f"[LOG CONSOLE IGNORED]: {clean_text}")
            continue

        accumulated += line
        try:
            return json.loads(accumulated)
        except json.JSONDecodeError:
            continue


def send_command(process, command):
    print(f"\n>>> {command}")
    process.stdin.write(command + "\n")
    process.stdin.flush()
    response = read_full_response(process)
    if response is None:
        raise RuntimeError("No response received (process may have terminated).")
    print(json.dumps(response, indent=2))
    return response


def fail(message, failures):
    print(f"[FAIL] {message}")
    failures.append(message)


def expect_ok(response, command, failures):
    if response.get("status") != "ok":
        fail(f"{command}: expected status=ok, got {response.get('status')}", failures)


def expect_error_with_hint_key(response, command, failures):
    if response.get("status") != "error":
        fail(f"{command}: expected status=error, got {response.get('status')}", failures)
        return
    if "hint" not in response:
        fail(f"{command}: expected 'hint' key in error response", failures)


def run_tests():
    failures = []
    print(f"Starting {VTX_CLI_PATH}...")

    try:
        process = subprocess.Popen(
            [VTX_CLI_PATH],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=sys.stderr,
            text=True,
            bufsize=1,
        )
    except FileNotFoundError:
        print(f"Error: Could not find the executable at {VTX_CLI_PATH}")
        sys.exit(1)

    try:
        print("=== STARTUP MESSAGE ===")
        startup = read_full_response(process)
        if startup is None:
            fail("No startup JSON received", failures)
            raise RuntimeError("Startup failed")
        print(json.dumps(startup, indent=2))

        if startup.get("status") != "ready":
            fail(f"startup status expected 'ready', got {startup.get('status')}", failures)

        print("\n=== EXECUTING COMMANDS ===")

        expect_ok(send_command(process, "help"), "help", failures)
        expect_ok(send_command(process, f"open {TEST_FILE} --json-only"), "open", failures)
        expect_ok(send_command(process, "info"), "info", failures)
        expect_ok(send_command(process, "header"), "header", failures)
        expect_ok(send_command(process, "footer"), "footer", failures)
        expect_ok(send_command(process, "chunks"), "chunks", failures)
        expect_ok(send_command(process, "events"), "events", failures)
        expect_ok(send_command(process, "frame"), "frame", failures)
        expect_ok(send_command(process, "frame 0"), "frame 0", failures)

        buckets_resp = send_command(process, "buckets")
        expect_ok(buckets_resp, "buckets", failures)
        bucket_name = None
        bucket_idx = 0
        try:
            bucket_list = buckets_resp["data"]["buckets"]
            if bucket_list:
                bucket_idx = bucket_list[0].get("index", 0)
                bucket_name = bucket_list[0].get("name")
        except Exception:
            fail("buckets: unexpected response shape", failures)

        # Bucket-by-name coverage (falls back to index if name is missing).
        bucket_arg = bucket_name if bucket_name else str(bucket_idx)
        entities_resp = send_command(process, f"entities {bucket_arg}")
        expect_ok(entities_resp, f"entities {bucket_arg}", failures)

        expect_ok(send_command(process, f"entity {bucket_idx} --index 0"), "entity --index", failures)
        expect_ok(
            send_command(process, f"property {bucket_idx} --index 0 UniqueId"),
            "property UniqueId",
            failures,
        )
        expect_ok(send_command(process, "types"), "types", failures)
        expect_ok(send_command(process, "diff 0 1"), "diff", failures)
        # Use a known property to avoid dataset-specific failures.
        expect_ok(send_command(process, "search UniqueId == __codex_no_match__"), "search", failures)
        expect_ok(send_command(process, f"track {bucket_idx} --index 0 UniqueId 0 10 1"), "track", failures)

        # Error-shape coverage: every error must include 'hint' key.
        expect_error_with_hint_key(
            send_command(process, f"property {bucket_idx} --index 0 UniqeId"),
            "property typo",
            failures,
        )
        expect_error_with_hint_key(send_command(process, "types RLFrameData"), "types missing", failures)
        expect_error_with_hint_key(send_command(process, "unknowncmd"), "unknowncmd", failures)

        expect_ok(send_command(process, "close"), "close", failures)
        expect_ok(send_command(process, "exit"), "exit", failures)

    finally:
        process.terminate()

    print("\n=== TESTS COMPLETED ===")
    if failures:
        print(f"Total failures: {len(failures)}")
        sys.exit(1)
    print("All checks passed.")


if __name__ == "__main__":
    run_tests()
