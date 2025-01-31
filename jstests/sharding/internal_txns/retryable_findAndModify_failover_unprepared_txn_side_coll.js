/*
 * Tests that unprepared retryable internal transactions for findAndModify on a shard with image
 * collection enabled are retryable across failover.
 *
 * @tags: [requires_fcv_53, featureFlagInternalTransactions]
 */
(function() {
'use strict';

load("jstests/sharding/internal_txns/libs/retryable_internal_transaction_test.js");

const transactionTest = new RetryableInternalTransactionTest();
transactionTest.runTestsForAllUnpreparedRetryableInternalTransactionTypes(
    transactionTest.runFindAndModifyTestsEnableImageCollection, transactionTest.TestMode.kFailover);
transactionTest.stop();
})();
