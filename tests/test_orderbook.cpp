#include <gtest/gtest.h>

#include "orderbook/OrderBook.hpp"

using namespace ob;

namespace {

Price px(const char* s) { return *parsePrice(s); }

// Every test goes through these wrappers, so checkInvariants() runs after
// every single operation.
class OrderBookTest : public ::testing::Test {
protected:
    ExecutionReport limit(OrderId id, Side side, Quantity qty, const char* price) {
        auto r = book.addLimit(id, side, qty, px(price));
        expectValid();
        return r;
    }
    ExecutionReport market(OrderId id, Side side, Quantity qty) {
        auto r = book.addMarket(id, side, qty);
        expectValid();
        return r;
    }
    ExecutionReport cancel(OrderId id) {
        auto r = book.cancel(id);
        expectValid();
        return r;
    }
    void expectValid() {
        std::string why;
        ASSERT_TRUE(book.checkInvariants(&why)) << why;
    }

    OrderBook book;
};

Trade trade(OrderId buy, OrderId sell, const char* price, Quantity qty) { return Trade{buy, sell, px(price), qty}; }

}  // namespace

TEST(PriceParsing, ParsesDecimalStringsIntoTicks) {
    EXPECT_EQ(parsePrice("101.50"), 10150);
    EXPECT_EQ(parsePrice("101.5"), 10150);
    EXPECT_EQ(parsePrice("101"), 10100);
    EXPECT_EQ(parsePrice("0.01"), 1);
    EXPECT_EQ(parsePrice(".5"), 50);
    EXPECT_EQ(parsePrice("-1.25"), -125);
    EXPECT_FALSE(parsePrice("1.234"));  // finer than one tick
    EXPECT_FALSE(parsePrice("abc"));
    EXPECT_FALSE(parsePrice(""));
    EXPECT_FALSE(parsePrice("."));
    EXPECT_FALSE(parsePrice("1.2.3"));
    EXPECT_FALSE(parsePrice("99999999999999999999"));  // overflow
    EXPECT_EQ(formatPrice(10150), "101.50");
    EXPECT_EQ(formatPrice(5), "0.05");
    EXPECT_EQ(formatPrice(-125), "-1.25");
}

TEST_F(OrderBookTest, EmptyBookHasNoBestPricesAndMarketOrderProducesNoTrades) {
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.bestAsk());
    auto r = market(1, Side::Buy, 100);
    EXPECT_TRUE(r.accepted);
    EXPECT_TRUE(r.trades.empty());
    EXPECT_EQ(r.filled, 0);
    EXPECT_EQ(r.remaining, 100);  // all of it cancelled
    EXPECT_EQ(book.orderCount(), 0u);
}

TEST_F(OrderBookTest, NonCrossingLimitOrderRests) {
    auto b = limit(1, Side::Buy, 10, "99.00");
    auto s = limit(2, Side::Sell, 10, "101.00");
    EXPECT_TRUE(b.accepted);
    EXPECT_TRUE(b.trades.empty());
    EXPECT_EQ(b.remaining, 10);
    EXPECT_TRUE(s.trades.empty());
    EXPECT_EQ(book.bestBid(), px("99.00"));
    EXPECT_EQ(book.bestAsk(), px("101.00"));
    EXPECT_EQ(book.orderCount(), 2u);
}

TEST_F(OrderBookTest, FullFill) {
    limit(1, Side::Sell, 50, "100.00");
    auto r = limit(2, Side::Buy, 50, "100.00");
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0], trade(2, 1, "100.00", 50));
    EXPECT_EQ(r.filled, 50);
    EXPECT_EQ(r.remaining, 0);
    EXPECT_EQ(book.orderCount(), 0u);
    EXPECT_FALSE(book.bestBid());
    EXPECT_FALSE(book.bestAsk());
}

TEST_F(OrderBookTest, PartialFillRemainderRests) {
    limit(1, Side::Sell, 30, "100.00");
    auto r = limit(2, Side::Buy, 50, "100.00");
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0], trade(2, 1, "100.00", 30));
    EXPECT_EQ(r.remaining, 20);
    EXPECT_EQ(book.bestBid(), px("100.00"));
    EXPECT_FALSE(book.bestAsk());
    EXPECT_EQ(book.restingQuantity(2), 20);
}

TEST_F(OrderBookTest, PartiallyFilledRestingOrderKeepsRemainder) {
    limit(1, Side::Sell, 100, "100.00");
    limit(2, Side::Buy, 40, "100.00");
    EXPECT_EQ(book.restingQuantity(1), 60);
    auto d = book.depth(5);
    ASSERT_EQ(d.asks.size(), 1u);
    EXPECT_EQ(d.asks[0].quantity, 60);
}

TEST_F(OrderBookTest, AggressiveOrderSweepsSeveralLevels) {
    limit(1, Side::Sell, 10, "100.00");
    limit(2, Side::Sell, 10, "100.50");
    limit(3, Side::Sell, 10, "101.00");
    limit(4, Side::Sell, 10, "102.00");
    auto r = limit(5, Side::Buy, 25, "101.00");
    ASSERT_EQ(r.trades.size(), 3u);
    EXPECT_EQ(r.trades[0], trade(5, 1, "100.00", 10));
    EXPECT_EQ(r.trades[1], trade(5, 2, "100.50", 10));
    EXPECT_EQ(r.trades[2], trade(5, 3, "101.00", 5));
    EXPECT_EQ(r.remaining, 0);
    EXPECT_EQ(book.bestAsk(), px("101.00"));
    EXPECT_EQ(book.restingQuantity(3), 5);
    EXPECT_EQ(book.askLevels(), 2u);
}

TEST_F(OrderBookTest, SweepStopsAtLimitPriceAndRestsRemainder) {
    limit(1, Side::Buy, 10, "100.00");
    limit(2, Side::Buy, 10, "99.00");
    limit(3, Side::Buy, 10, "98.00");
    auto r = limit(4, Side::Sell, 50, "99.00");
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0], trade(1, 4, "100.00", 10));
    EXPECT_EQ(r.trades[1], trade(2, 4, "99.00", 10));
    EXPECT_EQ(r.remaining, 30);
    EXPECT_EQ(book.bestAsk(), px("99.00"));
    EXPECT_EQ(book.bestBid(), px("98.00"));
}

TEST_F(OrderBookTest, PricePriorityBetterPriceMatchesFirst) {
    limit(1, Side::Sell, 10, "101.00");  // arrives first, worse price
    limit(2, Side::Sell, 10, "100.00");  // arrives second, better price
    auto r = limit(3, Side::Buy, 10, "101.00");
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].sellId, 2u);
    EXPECT_EQ(r.trades[0].price, px("100.00"));

    limit(4, Side::Buy, 10, "95.00");
    limit(5, Side::Buy, 10, "96.00");
    auto s = limit(6, Side::Sell, 10, "90.00");
    ASSERT_EQ(s.trades.size(), 1u);
    EXPECT_EQ(s.trades[0].buyId, 5u);
}

TEST_F(OrderBookTest, TimePriorityEarlierOrderAtSamePriceMatchesFirst) {
    limit(1, Side::Buy, 10, "100.00");
    limit(2, Side::Buy, 10, "100.00");
    limit(3, Side::Buy, 10, "100.00");
    auto r = limit(4, Side::Sell, 25, "100.00");
    ASSERT_EQ(r.trades.size(), 3u);
    EXPECT_EQ(r.trades[0].buyId, 1u);
    EXPECT_EQ(r.trades[1].buyId, 2u);
    EXPECT_EQ(r.trades[2].buyId, 3u);
    EXPECT_EQ(r.trades[2].quantity, 5);
    EXPECT_EQ(book.restingQuantity(3), 5);
}

TEST_F(OrderBookTest, TradePriceIsRestingOrdersPrice) {
    limit(1, Side::Sell, 10, "100.00");
    auto buy = limit(2, Side::Buy, 10, "105.00");
    ASSERT_EQ(buy.trades.size(), 1u);
    EXPECT_EQ(buy.trades[0].price, px("100.00"));

    limit(3, Side::Buy, 10, "100.00");
    auto sell = limit(4, Side::Sell, 10, "95.00");
    ASSERT_EQ(sell.trades.size(), 1u);
    EXPECT_EQ(sell.trades[0].price, px("100.00"));
}

TEST_F(OrderBookTest, MarketOrderRemainderIsCancelledAndNeverRests) {
    limit(1, Side::Sell, 10, "100.00");
    limit(2, Side::Sell, 10, "101.00");
    auto r = market(3, Side::Buy, 50);
    EXPECT_TRUE(r.accepted);
    ASSERT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.trades[0], trade(3, 1, "100.00", 10));
    EXPECT_EQ(r.trades[1], trade(3, 2, "101.00", 10));
    EXPECT_EQ(r.filled, 20);
    EXPECT_EQ(r.remaining, 30);
    EXPECT_FALSE(book.bestBid());  // nothing rested on the bid side
    EXPECT_FALSE(book.bestAsk());
    EXPECT_FALSE(book.restingQuantity(3));
}

TEST_F(OrderBookTest, MarketSellMatchesBidsAtAnyPrice) {
    limit(1, Side::Buy, 10, "1.00");
    auto r = market(2, Side::Sell, 5);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0], trade(1, 2, "1.00", 5));
    EXPECT_EQ(r.remaining, 0);
    EXPECT_EQ(book.restingQuantity(1), 5);
}

TEST_F(OrderBookTest, CancelRemovesRestingOrder) {
    limit(1, Side::Buy, 10, "100.00");
    limit(2, Side::Buy, 20, "100.00");
    auto r = cancel(1);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.remaining, 10);
    EXPECT_FALSE(book.restingQuantity(1));
    auto d = book.depth(1);
    ASSERT_EQ(d.bids.size(), 1u);
    EXPECT_EQ(d.bids[0].quantity, 20);
    EXPECT_EQ(d.bids[0].orders, 1u);

    // The cancelled order no longer matches.
    auto s = limit(3, Side::Sell, 5, "100.00");
    ASSERT_EQ(s.trades.size(), 1u);
    EXPECT_EQ(s.trades[0].buyId, 2u);
}

TEST_F(OrderBookTest, CancelOfPartiallyFilledOrderRemovesTheRemainder) {
    limit(1, Side::Sell, 100, "100.00");
    limit(2, Side::Buy, 30, "100.00");
    auto r = cancel(1);
    EXPECT_TRUE(r.accepted);
    EXPECT_EQ(r.remaining, 70);
    EXPECT_EQ(book.orderCount(), 0u);
}

TEST_F(OrderBookTest, CancelFailsOnUnknownId) {
    auto r = cancel(42);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.reason, RejectReason::UnknownId);
}

TEST_F(OrderBookTest, CancelFailsOnFilledOrder) {
    limit(1, Side::Sell, 10, "100.00");
    limit(2, Side::Buy, 10, "100.00");
    auto r1 = cancel(1);
    auto r2 = cancel(2);
    EXPECT_FALSE(r1.accepted);
    EXPECT_EQ(r1.reason, RejectReason::NotActive);
    EXPECT_FALSE(r2.accepted);
    EXPECT_EQ(r2.reason, RejectReason::NotActive);
}

TEST_F(OrderBookTest, CancelTwiceFailsTheSecondTime) {
    limit(1, Side::Sell, 10, "100.00");
    EXPECT_TRUE(cancel(1).accepted);
    auto r = cancel(1);
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.reason, RejectReason::NotActive);
}

TEST_F(OrderBookTest, DuplicateIdIsRejected) {
    limit(1, Side::Buy, 10, "100.00");
    auto r = limit(1, Side::Sell, 10, "200.00");
    EXPECT_FALSE(r.accepted);
    EXPECT_EQ(r.reason, RejectReason::DuplicateId);
    EXPECT_FALSE(book.bestAsk());

    // An id stays used even after its order is gone.
    cancel(1);
    EXPECT_EQ(limit(1, Side::Buy, 10, "100.00").reason, RejectReason::DuplicateId);
    market(2, Side::Buy, 10);
    EXPECT_EQ(market(2, Side::Buy, 10).reason, RejectReason::DuplicateId);
}

TEST_F(OrderBookTest, InvalidQuantityAndPriceAreRejected) {
    EXPECT_EQ(limit(1, Side::Buy, 0, "100.00").reason, RejectReason::InvalidQuantity);
    EXPECT_EQ(limit(2, Side::Buy, -5, "100.00").reason, RejectReason::InvalidQuantity);
    EXPECT_EQ(limit(3, Side::Buy, 10, "0").reason, RejectReason::InvalidPrice);
    EXPECT_EQ(limit(4, Side::Buy, 10, "-1.00").reason, RejectReason::InvalidPrice);
    EXPECT_EQ(market(5, Side::Sell, 0).reason, RejectReason::InvalidQuantity);
    EXPECT_EQ(book.orderCount(), 0u);
    // A rejected order does not use up its id.
    EXPECT_TRUE(limit(1, Side::Buy, 10, "100.00").accepted);
}

TEST_F(OrderBookTest, EmptyPriceLevelsAreRemoved) {
    limit(1, Side::Sell, 10, "100.00");
    limit(2, Side::Sell, 10, "101.00");
    EXPECT_EQ(book.askLevels(), 2u);
    limit(3, Side::Buy, 10, "100.00");  // fills the whole 100.00 level
    EXPECT_EQ(book.askLevels(), 1u);
    EXPECT_EQ(book.bestAsk(), px("101.00"));
    cancel(2);  // cancels the only order at 101.00
    EXPECT_EQ(book.askLevels(), 0u);
    EXPECT_FALSE(book.bestAsk());
}

TEST_F(OrderBookTest, DepthReportsTopLevelsBestFirst) {
    for (OrderId i = 1; i <= 7; ++i) {
        book.addLimit(i, Side::Buy, 10 * static_cast<Quantity>(i), 9000 + 100 * static_cast<Price>(i));
        book.addLimit(100 + i, Side::Sell, 5, 11000 + 100 * static_cast<Price>(i));
    }
    book.addLimit(50, Side::Buy, 1, 9700);  // second order on the 97.00 level
    expectValid();
    auto d = book.depth(5);
    ASSERT_EQ(d.bids.size(), 5u);
    ASSERT_EQ(d.asks.size(), 5u);
    EXPECT_EQ(d.bids[0].price, 9700);
    EXPECT_EQ(d.bids[0].quantity, 71);
    EXPECT_EQ(d.bids[0].orders, 2u);
    EXPECT_EQ(d.bids[4].price, 9300);
    EXPECT_EQ(d.asks[0].price, 11100);
    EXPECT_EQ(d.asks[4].price, 11500);
}

TEST_F(OrderBookTest, ExactCrossAtBestPriceKeepsBookUncrossed) {
    limit(1, Side::Buy, 10, "100.00");
    limit(2, Side::Sell, 5, "100.00");  // trades, does not rest
    EXPECT_EQ(book.bestBid(), px("100.00"));
    EXPECT_FALSE(book.bestAsk());
    limit(3, Side::Sell, 10, "100.01");
    EXPECT_EQ(*book.bestAsk() - *book.bestBid(), 1);
}
