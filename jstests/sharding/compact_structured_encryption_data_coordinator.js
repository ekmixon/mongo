// Basic test that the CompactStructuredEncryptionDataCoordinator runs.
// @tags: [requires_sharding,requires_fcv_60]

(function() {
'use strict';

load('jstests/fle2/libs/encrypted_client_util.js');

if (!TestData.setParameters.featureFlagFLE2) {
    jsTest.log('Skipping test because feature flag is not enabled');
    return;
}

const options = {
    mongos: 1,
    config: 1,
    shards: 1,
    rs: {nodes: [{}]},
};

const kHaveAuditing = buildInfo().modules.includes("enterprise");
if (kHaveAuditing) {
    jsTest.log('Including test for audit events since this is an enterprise build');
    const nodeOpts = options.rs.nodes[0];
    nodeOpts.auditDestination = 'file';
    nodeOpts.auditPath = MongoRunner.dataPath + '/audit.log';
    nodeOpts.auditFormat = 'JSON';
}

const st = new ShardingTest(options);

const admin = st.s0.getDB('admin');

// Setup collection with encrypted fields and a compactible metadata collection.
const encryptedFields = {
    fields: [
        {path: "firstName", bsonType: "string", queries: {"queryType": "equality"}},
        {path: "a.b.c", bsonType: "int", queries: {"queryType": "equality"}},
    ]
};

const client = new EncryptedClient(st.s0, 'test');
const test = client.getDB('test');

assert.commandWorked(
    client.createEncryptionCollection("encrypted", {encryptedFields: encryptedFields}));
assert.commandWorked(test.createCollection("unencrypted"));

assert.commandFailedWithCode(test.unencrypted.compact(), ErrorCodes.BadValue);

const reply = assert.commandWorked(test.encrypted.compact());
jsTest.log(reply);

// Validate dummy data we expect the placeholder compaction algorithm to return.
assert.eq(reply.stats.ecoc.read, 1);
assert.eq(reply.stats.ecoc.deleted, 2);

assert.eq(reply.stats.ecc.read, 3);
assert.eq(reply.stats.ecc.inserted, 4);
assert.eq(reply.stats.ecc.updated, 5);
assert.eq(reply.stats.ecc.deleted, 6);

assert.eq(reply.stats.esc.read, 7);
assert.eq(reply.stats.esc.inserted, 8);
assert.eq(reply.stats.esc.updated, 9);
assert.eq(reply.stats.esc.deleted, 10);

// The eccoc collection is gone, so we should return quickly with no work done.
const nowork = assert.commandWorked(test.encrypted.compact());
jsTest.log(nowork);

assert.eq(nowork.stats.ecoc.read, 0);
assert.eq(nowork.stats.ecoc.deleted, 0);

assert.eq(nowork.stats.ecc.read, 0);
assert.eq(nowork.stats.ecc.inserted, 0);
assert.eq(nowork.stats.ecc.updated, 0);
assert.eq(nowork.stats.ecc.deleted, 0);

assert.eq(nowork.stats.esc.read, 0);
assert.eq(nowork.stats.esc.inserted, 0);
assert.eq(nowork.stats.esc.updated, 0);
assert.eq(nowork.stats.esc.deleted, 0);

if (kHaveAuditing) {
    jsTest.log('Verifying audit contents');

    // Check the audit log for the rename/drop events.
    const audit = cat(options.rs.nodes[0].auditPath)
                      .split('\n')
                      .filter((l) => l !== '')
                      .map((l) => JSON.parse(l));
    jsTest.log('Audit Log: ' + tojson(audit));

    const renameEvents = audit.filter((ev) => (ev.atype === 'renameCollection') &&
                                          (ev.param.old === 'test.fle2.encrypted.ecoc'));
    assert.eq(renameEvents.length, 1, 'Invalid number of rename events: ' + tojson(renameEvents));
    assert.eq(renameEvents[0].result, ErrorCodes.OK);
    const tempNSS = renameEvents[0].param.new;

    const dropEvents =
        audit.filter((ev) => (ev.atype === 'dropCollection') && (ev.param.ns === tempNSS));
    assert.eq(dropEvents.length, 1, 'Invalid number of drop events: ' + tojson(dropEvents));
    assert.eq(dropEvents[0].result, ErrorCodes.OK);
}

st.stop();

jsTest.log(reply);
})();
