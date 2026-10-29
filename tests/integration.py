"""Real HTTP + PostgreSQL contracts. Requires an isolated database with sql/001_schema.sql."""
import argparse
import concurrent.futures
import json
import os
from pathlib import Path
import socket
import subprocess
import time
import unittest
import urllib.error
import urllib.request
import uuid


class LedgerContracts(unittest.TestCase):
    base = ""
    token = "integration-token-for-local-tests"

    def request(self, method, path, data=None, key=None, auth=True):
        headers = {"Content-Type": "application/json"}
        if auth:
            headers["Authorization"] = "Bearer " + self.token
        if key:
            headers["Idempotency-Key"] = key
        req = urllib.request.Request(self.base + path, method=method, headers=headers,
                                     data=None if data is None else json.dumps(data).encode())
        try:
            with urllib.request.urlopen(req, timeout=15) as response:
                return response.status, json.load(response)
        except urllib.error.HTTPError as error:
            return error.code, json.load(error)

    def account(self, balance=0):
        identifier = "a_" + uuid.uuid4().hex
        status, result = self.request("POST", "/v1/accounts", {"id": identifier, "opening_balance": balance})
        self.assertEqual(status, 201, result)
        return identifier

    def balance(self, identifier):
        status, result = self.request("GET", "/v1/accounts/" + identifier)
        self.assertEqual(status, 200, result)
        return result["balance"]

    def transfer(self, source, target, amount, key=None):
        return self.request("POST", "/v1/transfers", {"from": source, "to": target, "amount": amount}, key or uuid.uuid4().hex)

    def test_auth_and_validation(self):
        self.assertEqual(self.request("GET", "/v1/accounts/x", auth=False)[0], 401)
        for opening in [-1, 1.2, True, 9000000000000001, 18446744073709551615]:
            self.assertEqual(self.request("POST", "/v1/accounts", {"id": "bad", "opening_balance": opening})[0], 400)
        self.assertEqual(self.request("POST", "/v1/accounts", {"id": "bad'--", "opening_balance": 0})[0], 400)
        a, b = self.account(100), self.account()
        self.assertEqual(self.request("POST", "/v1/transfers", {"from": a, "to": b, "amount": 1})[0], 400)
        for amount in [0, -1, 1.1, True, 1000000000001]:
            self.assertEqual(self.transfer(a, b, amount)[0], 400)
        self.assertEqual(self.transfer(a, a, 1)[0], 400)
        self.assertEqual(self.transfer(a, "missing", 1)[0], 404)
        self.assertEqual(self.balance(a), 100)

    def test_idempotency_and_entries(self):
        a, b = self.account(100), self.account()
        key = uuid.uuid4().hex
        self.assertEqual(self.transfer(a, b, 40, key)[0], 201)
        status, replay = self.transfer(a, b, 40, key)
        self.assertEqual(status, 200)
        self.assertTrue(replay["replayed"])
        self.assertEqual(self.transfer(a, b, 41, key)[0], 409)
        self.assertEqual((self.balance(a), self.balance(b)), (60, 40))
        debit = self.request("GET", f"/v1/accounts/{a}/entries")[1]["entries"]
        credit = self.request("GET", f"/v1/accounts/{b}/entries")[1]["entries"]
        self.assertEqual(debit, [{"transfer_key": key, "delta": -40}])
        self.assertEqual(credit, [{"transfer_key": key, "delta": 40}])

    def test_failed_transfer_does_not_reserve_key(self):
        a, b, funding = self.account(), self.account(), self.account(100)
        key = uuid.uuid4().hex
        self.assertEqual(self.transfer(a, b, 50, key)[0], 409)
        self.assertEqual(self.transfer(funding, a, 100)[0], 201)
        self.assertEqual(self.transfer(a, b, 50, key)[0], 201)
        self.assertEqual((self.balance(a), self.balance(b)), (50, 50))

    def test_overflow_rolls_back(self):
        a, b = self.account(50), self.account(9000000000000000)
        self.assertEqual(self.transfer(a, b, 1)[0], 409)
        self.assertEqual((self.balance(a), self.balance(b)), (50, 9000000000000000))

    def test_database_failure_after_debit_rolls_back_everything(self):
        a, b = self.account(100), self.account()
        name = "test_" + uuid.uuid4().hex
        def sql(statement):
            return subprocess.check_output(["psql", os.environ["DATABASE_URL"], "-v", "ON_ERROR_STOP=1", "-Atc", statement], text=True).strip()
        # The second UPDATE violates this test-only constraint after the debit ran.
        sql(f"ALTER TABLE accounts ADD CONSTRAINT {name} CHECK (id <> '{b}' OR balance = 0)")
        key = uuid.uuid4().hex
        try:
            self.assertEqual(self.transfer(a, b, 10, key)[0], 409)
            self.assertEqual((self.balance(a), self.balance(b)), (100, 0))
            self.assertEqual(sql(f"SELECT count(*) FROM transfers WHERE idempotency_key='{key}'"), "0")
            self.assertEqual(sql(f"SELECT count(*) FROM entries WHERE transfer_key='{key}'"), "0")
        finally:
            sql(f"ALTER TABLE accounts DROP CONSTRAINT {name}")
        self.assertEqual(self.transfer(a, b, 10, key)[0], 201)

    def test_concurrent_overspend(self):
        a, b = self.account(100), self.account()
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(lambda _: self.transfer(a, b, 10)[0], range(30)))
        self.assertEqual(statuses.count(201), 10, statuses)
        self.assertEqual(statuses.count(409), 20, statuses)
        self.assertEqual((self.balance(a), self.balance(b)), (0, 100))

    def test_concurrent_same_key(self):
        a, b = self.account(100), self.account()
        key = uuid.uuid4().hex
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(lambda _: self.transfer(a, b, 10, key)[0], range(12)))
        self.assertEqual(statuses.count(201), 1, statuses)
        self.assertEqual(statuses.count(200), 11, statuses)
        self.assertEqual((self.balance(a), self.balance(b)), (90, 10))

    def test_opposite_transfers_preserve_total(self):
        a, b = self.account(10000), self.account(10000)
        def run(i):
            return self.transfer(a, b, 7)[0] if i % 2 else self.transfer(b, a, 7)[0]
        with concurrent.futures.ThreadPoolExecutor(max_workers=8) as executor:
            statuses = list(executor.map(run, range(40)))
        self.assertEqual(set(statuses), {201}, statuses)
        self.assertEqual((self.balance(a), self.balance(b)), (10000, 10000))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    args = parser.parse_args()
    if not os.environ.get("DATABASE_URL"):
        raise SystemExit("DATABASE_URL is required; use an isolated test database")
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    LedgerContracts.base = f"http://127.0.0.1:{port}"
    environment = {**os.environ, "PORT": str(port), "HOST": "127.0.0.1", "API_TOKEN": LedgerContracts.token}
    log = Path("ledger-test-server.log")
    with log.open("w") as output:
        process = subprocess.Popen([args.server], env=environment, stdout=output, stderr=output)
        try:
            ready = False
            for _ in range(100):
                if process.poll() is not None:
                    raise RuntimeError("Server exited: " + log.read_text())
                try:
                    with urllib.request.urlopen(LedgerContracts.base + "/health", timeout=2) as response:
                        ready = response.status == 200
                except (urllib.error.URLError, TimeoutError):
                    pass
                if ready:
                    break
                time.sleep(0.1)
            if not ready:
                raise RuntimeError("Database/server readiness timed out")
            result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(LedgerContracts))
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
    raise SystemExit(0 if result.wasSuccessful() else 1)


if __name__ == "__main__":
    main()
