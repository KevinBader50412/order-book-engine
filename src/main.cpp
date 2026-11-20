// Command-line front end for the matching engine.
//
//   ./engine                     interactive: read commands from stdin
//   ./engine --replay FILE       replay commands from a file
//
// The main thread reads and parses lines and submits them to the engine. The
// engine's matching thread applies them and prints the results, so output is
// always in processing order.

#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unistd.h>

#include "orderbook/CommandParser.hpp"
#include "orderbook/MatchingEngine.hpp"

namespace {

void printUsage(const char* prog) {
    std::cerr << "usage: " << prog << " [--replay FILE]\n\n"
              << "commands:\n"
              << "  ADD <id> BUY|SELL LIMIT <qty> <price>\n"
              << "  ADD <id> BUY|SELL MARKET <qty>\n"
              << "  CANCEL <id>\n"
              << "  PRINT\n"
              << "  QUIT\n";
}

void printReport(const ob::Command& cmd, const ob::ExecutionReport& r) {
    if (!r.accepted) {
        std::cout << "REJECT id=" << r.id << " reason=" << ob::toString(r.reason) << '\n';
        return;
    }
    if (cmd.kind == ob::CommandKind::Cancel) {
        std::cout << "CANCELLED id=" << r.id << " qty=" << r.remaining << '\n';
    } else if (cmd.type == ob::OrderType::Limit) {
        std::cout << "ACCEPTED id=" << r.id << " filled=" << r.filled << " resting=" << r.remaining << '\n';
    } else {
        std::cout << "ACCEPTED id=" << r.id << " filled=" << r.filled << " unfilled=" << r.remaining
                  << (r.remaining > 0 ? " (cancelled, market orders never rest)" : "") << '\n';
    }
}

void printBook(const ob::OrderBook& book) {
    constexpr std::size_t kLevels = 5;
    const auto d = book.depth(kLevels);
    std::ostringstream out;
    out << "------------ ORDER BOOK ------------\n";
    out << "  side      price        qty  orders\n";
    // Asks from the 5th best down to the best, so the spread sits in the middle.
    for (auto it = d.asks.rbegin(); it != d.asks.rend(); ++it) {
        out << "  ASK  " << std::setw(10) << ob::formatPrice(it->price) << std::setw(11) << it->quantity
            << std::setw(8) << it->orders << '\n';
    }
    out << "  ----------------------------------\n";
    for (const auto& lvl : d.bids) {
        out << "  BID  " << std::setw(10) << ob::formatPrice(lvl.price) << std::setw(11) << lvl.quantity
            << std::setw(8) << lvl.orders << '\n';
    }
    const auto bid = book.bestBid();
    const auto ask = book.bestAsk();
    out << "best_bid=" << (bid ? ob::formatPrice(*bid) : "none") << " best_ask=" << (ask ? ob::formatPrice(*ask) : "none")
        << " spread=" << (bid && ask ? ob::formatPrice(*ask - *bid) : "n/a") << '\n';
    std::cout << out.str();
}

}  // namespace

int main(int argc, char** argv) {
    std::ifstream file;
    std::istream* in = &std::cin;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--replay") == 0 && i + 1 < argc) {
            file.open(argv[++i]);
            if (!file) {
                std::cerr << "cannot open " << argv[i] << '\n';
                return 1;
            }
            in = &file;
        } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        } else {
            printUsage(argv[0]);
            return 2;
        }
    }

    const bool interactive = in == &std::cin && isatty(STDIN_FILENO);
    if (interactive) std::cerr << "order-book-engine: type commands, QUIT to exit\n";

    ob::MatchingEngine engine;
    engine.setTradeListener([](const ob::Trade& t) { std::cout << t << '\n'; });
    engine.setReportListener(printReport);
    engine.setBookListener(printBook);
    engine.start();

    std::string line;
    std::size_t lineNo = 0;
    while (std::getline(*in, line)) {
        ++lineNo;
        auto parsed = ob::parseCommand(line);
        if (!parsed.ok()) {
            std::cerr << "ERROR line " << lineNo << ": " << parsed.error << '\n';
            continue;
        }
        if (!parsed.command) continue;  // blank line or comment
        if (parsed.command->kind == ob::CommandKind::Quit) break;
        engine.submit(*parsed.command);
    }

    engine.stop();  // drains the queue, then joins the matching thread
    std::cout.flush();
    return 0;
}
