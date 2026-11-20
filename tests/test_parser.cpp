#include <gtest/gtest.h>

#include "orderbook/CommandParser.hpp"

using namespace ob;

TEST(CommandParser, ParsesLimitOrder) {
    auto r = parseCommand("ADD 12 BUY LIMIT 30 101.50");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_TRUE(r.command);
    EXPECT_EQ(r.command->kind, CommandKind::Add);
    EXPECT_EQ(r.command->id, 12u);
    EXPECT_EQ(r.command->side, Side::Buy);
    EXPECT_EQ(r.command->type, OrderType::Limit);
    EXPECT_EQ(r.command->quantity, 30);
    EXPECT_EQ(r.command->price, 10150);
}

TEST(CommandParser, ParsesMarketOrderCaseInsensitively) {
    auto r = parseCommand("  add 7 sell market 5   # trailing comment");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.command->side, Side::Sell);
    EXPECT_EQ(r.command->type, OrderType::Market);
    EXPECT_EQ(r.command->quantity, 5);
}

TEST(CommandParser, ParsesCancelPrintQuit) {
    EXPECT_EQ(parseCommand("CANCEL 3").command->kind, CommandKind::Cancel);
    EXPECT_EQ(parseCommand("CANCEL 3").command->id, 3u);
    EXPECT_EQ(parseCommand("PRINT").command->kind, CommandKind::Print);
    EXPECT_EQ(parseCommand("QUIT").command->kind, CommandKind::Quit);
}

TEST(CommandParser, BlankLinesAndCommentsProduceNoCommand) {
    for (const char* line : {"", "   ", "# just a comment"}) {
        auto r = parseCommand(line);
        EXPECT_TRUE(r.ok());
        EXPECT_FALSE(r.command);
    }
}

TEST(CommandParser, NegativeQuantityParsesSoTheBookCanRejectIt) {
    auto r = parseCommand("ADD 1 BUY LIMIT -5 100");
    ASSERT_TRUE(r.ok());
    EXPECT_EQ(r.command->quantity, -5);
}

TEST(CommandParser, RejectsMalformedLines) {
    for (const char* line : {
             "FOO",
             "ADD",
             "ADD x BUY LIMIT 1 100",
             "ADD 1 HOLD LIMIT 1 100",
             "ADD 1 BUY STOP 1 100",
             "ADD 1 BUY LIMIT ten 100",
             "ADD 1 BUY LIMIT 10",
             "ADD 1 BUY LIMIT 10 1.234",
             "ADD 1 BUY MARKET 10 100",
             "CANCEL",
             "CANCEL abc",
             "PRINT now",
         }) {
        auto r = parseCommand(line);
        EXPECT_FALSE(r.ok()) << "should reject: " << line;
        EXPECT_FALSE(r.command);
    }
}
