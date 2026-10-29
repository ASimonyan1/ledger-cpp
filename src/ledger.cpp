#include "ledger/ledger.hpp"
#include <libpq-fe.h>
#include <algorithm>
#include <charconv>
#include <memory>
#include <utility>

namespace ledger {
namespace {
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;
class Connection {
public:
    explicit Connection(const std::string& dsn) : handle_(PQconnectdb(dsn.c_str()), PQfinish) {
        if (!handle_ || PQstatus(handle_.get()) != CONNECTION_OK) throw Error(503, "database_unavailable");
        exec("SET statement_timeout = '3000ms'");
        exec("SET lock_timeout = '2000ms'");
    }
    Result exec(const std::string& sql, const std::vector<std::string>& params = {}) {
        std::vector<const char*> values;
        for (const auto& p : params) values.push_back(p.c_str());
        Result result(PQexecParams(handle_.get(), sql.c_str(), static_cast<int>(values.size()),
                                  nullptr, values.data(), nullptr, nullptr, 0), PQclear);
        if (!result) throw Error(503, "database_unavailable");
        const auto status = PQresultStatus(result.get());
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            const char* field = PQresultErrorField(result.get(), PG_DIAG_SQLSTATE);
            const std::string code = field ? field : "";
            if (code == "23505") throw Error(409, "already_exists");
            if (code == "23503") throw Error(404, "account_not_found");
            if (code == "23514") throw Error(409, "balance_constraint");
            // Connection loss after COMMIT is ambiguous: retry the same idempotency key.
            throw Error(503, "database_unavailable_retry_same_key");
        }
        return result;
    }
    void rollback() noexcept { Result ignored(PQexec(handle_.get(), "ROLLBACK"), PQclear); }
private:
    std::unique_ptr<PGconn, decltype(&PQfinish)> handle_;
};
class Transaction {
public:
    explicit Transaction(Connection& c) : c_(c) { c_.exec("BEGIN"); }
    ~Transaction() { if (!committed_) c_.rollback(); }
    void commit() { c_.exec("COMMIT"); committed_ = true; }
private:
    Connection& c_;
    bool committed_ = false;
};
std::int64_t number(const char* value) {
    std::int64_t result{};
    const std::string text(value);
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data()+text.size()) throw Error(503, "invalid_database_value");
    return result;
}
void require_id(const std::string& id) { if (!valid_id(id)) throw Error(400, "invalid_identifier"); }
}
bool valid_id(const std::string& value) {
    return !value.empty() && value.size() <= 80 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}
Ledger::Ledger(std::string dsn) : connection_string_(std::move(dsn)) {}
bool Ledger::healthy() const {
    try { Connection c(connection_string_); c.exec("SELECT 1 FROM accounts LIMIT 0"); return true; }
    catch (...) { return false; }
}
Account Ledger::create_account(const std::string& id, std::int64_t opening) const {
    require_id(id);
    if (opening < 0 || opening > max_balance) throw Error(400, "invalid_opening_balance");
    Connection c(connection_string_);
    c.exec("INSERT INTO accounts(id, opening_balance, balance) VALUES($1,$2::bigint,$2::bigint)", {id, std::to_string(opening)});
    return {id, opening};
}
Account Ledger::account(const std::string& id) const {
    require_id(id); Connection c(connection_string_);
    auto r = c.exec("SELECT balance FROM accounts WHERE id=$1", {id});
    if (PQntuples(r.get()) == 0) throw Error(404, "account_not_found");
    return {id, number(PQgetvalue(r.get(), 0, 0))};
}
Transfer Ledger::transfer(const std::string& key, const std::string& from,
                          const std::string& to, std::int64_t amount) const {
    require_id(key); require_id(from); require_id(to);
    if (from == to || amount <= 0 || amount > max_transfer) throw Error(400, "invalid_transfer");
    Connection c(connection_string_); Transaction tx(c);
    // Reserve the key before changing balances. A concurrent retry waits for commit/rollback.
    auto inserted = c.exec("INSERT INTO transfers(idempotency_key,from_id,to_id,amount) "
        "VALUES($1,$2,$3,$4::bigint) ON CONFLICT(idempotency_key) DO NOTHING RETURNING idempotency_key",
        {key, from, to, std::to_string(amount)});
    if (PQntuples(inserted.get()) == 0) {
        auto old = c.exec("SELECT from_id,to_id,amount FROM transfers WHERE idempotency_key=$1", {key});
        if (PQntuples(old.get()) != 1) throw Error(503, "retry_same_key");
        if (from != PQgetvalue(old.get(),0,0) || to != PQgetvalue(old.get(),0,1) || amount != number(PQgetvalue(old.get(),0,2)))
            throw Error(409, "idempotency_conflict");
        tx.commit(); return {key, from, to, amount, true};
    }
    // Stable order prevents opposite-direction transfer deadlocks. NO KEY UPDATE
    // remains compatible with FK KEY SHARE locks acquired by the INSERT above.
    auto accounts = c.exec("SELECT id,balance FROM accounts WHERE id IN($1,$2) ORDER BY id FOR NO KEY UPDATE", {from,to});
    if (PQntuples(accounts.get()) != 2) throw Error(404, "account_not_found");
    std::int64_t source=0, destination=0;
    for (int i=0; i<2; ++i) {
        if (from == PQgetvalue(accounts.get(),i,0)) source=number(PQgetvalue(accounts.get(),i,1));
        else destination=number(PQgetvalue(accounts.get(),i,1));
    }
    if (source < amount) throw Error(409, "insufficient_funds");
    if (destination > max_balance-amount) throw Error(409, "balance_limit");
    c.exec("UPDATE accounts SET balance=balance-$2::bigint WHERE id=$1", {from,std::to_string(amount)});
    c.exec("UPDATE accounts SET balance=balance+$2::bigint WHERE id=$1", {to,std::to_string(amount)});
    c.exec("INSERT INTO entries(transfer_key,account_id,delta) VALUES($1,$2,-$4::bigint),($1,$3,$4::bigint)",
           {key,from,to,std::to_string(amount)});
    tx.commit(); return {key,from,to,amount,false};
}
std::vector<Entry> Ledger::entries(const std::string& id) const {
    require_id(id); Connection c(connection_string_);
    auto exists=c.exec("SELECT 1 FROM accounts WHERE id=$1", {id});
    if (PQntuples(exists.get())==0) throw Error(404,"account_not_found");
    auto r=c.exec("SELECT transfer_key,delta FROM entries WHERE account_id=$1 ORDER BY transfer_key LIMIT 100", {id});
    std::vector<Entry> result;
    for(int i=0;i<PQntuples(r.get());++i) result.push_back({PQgetvalue(r.get(),i,0),number(PQgetvalue(r.get(),i,1))});
    return result;
}
}
