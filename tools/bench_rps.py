import argparse
import asyncio
import json
import sys
import time
from typing import Any


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="JSON-RPC TCP benchmark (newline-delimited JSON).",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Server host (default: 127.0.0.1).")
    parser.add_argument("--port", type=int, default=8080, help="Server port (default: 8080).")
    parser.add_argument("--connections", type=int, default=50, help="Parallel TCP connections (default: 50).")
    parser.add_argument("--duration", type=float, default=5.0, help="Benchmark duration in seconds (default: 5).")
    parser.add_argument("--timeout", type=float, default=5.0, help="Per-request read timeout in seconds (default: 5).")
    parser.add_argument("--method", default="ping", help="JSON-RPC method to call (default: ping).")
    parser.add_argument(
        "--params",
        default=None,
        help="Optional JSON params (array or object), e.g. '[1,2]' or '{\"x\":1}'.",
    )
    return parser.parse_args()


def build_request(method: str, params: Any, request_id: int) -> bytes:
    payload: dict[str, Any] = {"jsonrpc": "2.0", "id": request_id, "method": method}
    if params is not None:
        payload["params"] = params
    return (json.dumps(payload, separators=(",", ":")) + "\n").encode("utf-8")


async def worker(
    name: int,
    host: str,
    port: int,
    method: str,
    params: Any,
    deadline: float,
    timeout: float,
) -> int:
    try:
        reader, writer = await asyncio.open_connection(host, port)
    except OSError as exc:
        print(f"worker {name}: connection failed: {exc}", file=sys.stderr)
        return 0

    count = 0
    request_id = 0
    try:
        while time.perf_counter() < deadline:
            request_id += 1
            writer.write(build_request(method, params, request_id))
            await writer.drain()
            try:
                line = await asyncio.wait_for(reader.readline(), timeout=timeout)
            except TimeoutError:
                print(f"worker {name}: timeout waiting for response", file=sys.stderr)
                break
            if not line:
                print(f"worker {name}: server closed connection", file=sys.stderr)
                break
            count += 1
    finally:
        writer.close()
        try:
            await writer.wait_closed()
        except Exception:
            pass

    return count


async def run_benchmark(args: argparse.Namespace) -> int:
    if args.connections <= 0:
        print("--connections must be > 0", file=sys.stderr)
        return 2
    if args.duration <= 0:
        print("--duration must be > 0", file=sys.stderr)
        return 2
    if args.timeout <= 0:
        print("--timeout must be > 0", file=sys.stderr)
        return 2

    params = None
    if args.params is not None:
        try:
            params = json.loads(args.params)
        except json.JSONDecodeError as exc:
            print(f"--params must be valid JSON: {exc}", file=sys.stderr)
            return 2

    start = time.perf_counter()
    deadline = start + args.duration
    tasks = [
        asyncio.create_task(
            worker(
                idx,
                args.host,
                args.port,
                args.method,
                params,
                deadline,
                args.timeout,
            )
        )
        for idx in range(args.connections)
    ]

    counts = await asyncio.gather(*tasks)
    end = time.perf_counter()

    total = sum(counts)
    elapsed = end - start
    rps = total / elapsed if elapsed > 0 else 0.0

    print(f"connections={args.connections}")
    print(f"responses={total}")
    print(f"elapsed_sec={elapsed:.3f}")
    print(f"rps={rps:.1f}")

    return 0


def main() -> int:
    args = parse_args()
    return asyncio.run(run_benchmark(args))


if __name__ == "__main__":
    raise SystemExit(main())
