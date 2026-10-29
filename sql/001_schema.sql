BEGIN;
CREATE TABLE accounts (
    id text PRIMARY KEY CHECK (id ~ '^[A-Za-z0-9_-]{1,80}$'),
    opening_balance bigint NOT NULL CHECK (opening_balance BETWEEN 0 AND 9000000000000000),
    balance bigint NOT NULL CHECK (balance BETWEEN 0 AND 9000000000000000)
);
CREATE TABLE transfers (
    idempotency_key text PRIMARY KEY CHECK (idempotency_key ~ '^[A-Za-z0-9_-]{1,80}$'),
    from_id text NOT NULL REFERENCES accounts(id),
    to_id text NOT NULL REFERENCES accounts(id),
    amount bigint NOT NULL CHECK (amount BETWEEN 1 AND 1000000000000),
    created_at timestamptz NOT NULL DEFAULT now(),
    CHECK (from_id <> to_id)
);
CREATE TABLE entries (
    transfer_key text NOT NULL REFERENCES transfers(idempotency_key),
    account_id text NOT NULL REFERENCES accounts(id),
    delta bigint NOT NULL CHECK (delta <> 0),
    PRIMARY KEY (transfer_key,account_id)
);
CREATE INDEX entries_account_key ON entries(account_id,transfer_key);
COMMIT;
