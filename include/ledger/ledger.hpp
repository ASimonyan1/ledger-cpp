#pragma once
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace ledger {
inline constexpr std::int64_t max_balance = 9'000'000'000'000'000;
inline constexpr std::int64_t max_transfer = 1'000'000'000'000;
class Error : public std::runtime_error {
public:
    Error(int status, std::string code) : std::runtime_error(code), status(status) {}
    int status;
};
struct Account { std::string id; std::int64_t balance; };
struct Transfer {
    std::string key, from, to;
    std::int64_t amount;
    bool replayed = false;
};
struct Entry { std::string key; std::int64_t delta; };
bool valid_id(const std::string& value);
class Ledger {
public:
    explicit Ledger(std::string connection_string);
    bool healthy() const;
    Account create_account(const std::string& id, std::int64_t opening) const;
    Account account(const std::string& id) const;
    Transfer transfer(const std::string& key, const std::string& from,
                      const std::string& to, std::int64_t amount) const;
    std::vector<Entry> entries(const std::string& id) const;
private:
    std::string connection_string_;
};
}
