/**
 *    Copyright (C) 2022-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/matcher/implicit_validator.h"
#include "mongo/db/pipeline/expression_context_for_test.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"

namespace mongo {

namespace {

FleBlobHeader makeFleHeader(const EncryptedField& field, EncryptedBinDataType subtype) {
    FleBlobHeader blob;
    blob.fleBlobSubtype = static_cast<uint8_t>(subtype);
    memset(blob.keyUUID, 0, sizeof(blob.keyUUID));
    blob.originalBsonType = typeFromName(field.getBsonType());
    return blob;
}

BSONBinData makeFleBinData(const FleBlobHeader& blob) {
    return BSONBinData(
        reinterpret_cast<const void*>(&blob), sizeof(FleBlobHeader), BinDataType::Encrypt);
}

const auto kTestKeyId = UUID::parse("deadbeef-0000-0000-0000-0000deadbeef").getValue();
const EncryptedField kFieldAbc(kTestKeyId, "a.b.c", "string");
const EncryptedField kFieldAbd(kTestKeyId, "a.b.d", "int");
const EncryptedField kFieldC(kTestKeyId, "c", "array");
const EncryptedField kFieldAxy(kTestKeyId, "a.x.y", "bool");
const auto kValueAbc = makeFleHeader(kFieldAbc, EncryptedBinDataType::kFLE2EqualityIndexedValue);
const auto kValueAbd = makeFleHeader(kFieldAbd, EncryptedBinDataType::kFLE2EqualityIndexedValue);
const auto kValueC = makeFleHeader(kFieldC, EncryptedBinDataType::kFLE2EqualityIndexedValue);
const auto kValueAxy = makeFleHeader(kFieldAxy, EncryptedBinDataType::kFLE2EqualityIndexedValue);
const auto kValueFLE1 = makeFleHeader(kFieldAbc, EncryptedBinDataType::kDeterministic);
const auto kEncryptedFields = std::vector<EncryptedField>{kFieldAbc, kFieldAbd, kFieldC, kFieldAxy};

void replace_str(std::string& str, StringData search, StringData replace) {
    auto pos = str.find(search.rawData(), 0, search.size());
    if (pos == std::string::npos)
        return;
    str.replace(pos, search.size(), replace.rawData(), replace.size());
}

void replace_all(std::string& str, StringData search, StringData replace) {
    auto pos = str.find(search.rawData(), 0, search.size());
    while (pos != std::string::npos) {
        str.replace(pos, search.size(), replace.rawData(), replace.size());
        pos += replace.size();
        pos = str.find(search.rawData(), pos, search.size());
    }
}

std::string expectedLeafExpr(const EncryptedField& field) {
    std::string tmpl = R"(
            {"$or":[
                {"<NAME>":{"$not":{"$exists":true}}},
                {"$and":[
                    {"<NAME>":{"$_internalSchemaBinDataFLE2EncryptedType":[{"$numberInt":"<TYPE>"}]}}
                ]}
            ]})";
    FieldRef ref(field.getPath());
    replace_all(tmpl, "<NAME>"_sd, ref.getPart(ref.numParts() - 1));
    replace_all(
        tmpl, "<TYPE>"_sd, std::to_string(static_cast<int>(typeFromName(field.getBsonType()))));
    return tmpl;
}

std::string expectedNonLeafExpr(StringData fieldName, StringData subschema) {
    std::string tmpl = R"(
            {"$or":[
                {"<NAME>":{"$not":{"$exists":true}}},
                {"$and":[
                    {"$or":[
                        {"<NAME>":{"$not":{"$_internalSchemaType":[{"$numberInt":"3"}]}}},
                        {"<NAME>":{"$_internalSchemaObjectMatch":<SUBSCHEMA>}}
                    ]},
                    {"<NAME>":{"$not":{"$_internalSchemaType":[{"$numberInt":"4"}]}}}
                ]}
            ]})";
    replace_all(tmpl, "<NAME>"_sd, fieldName);
    replace_all(tmpl, "<SUBSCHEMA>"_sd, subschema);
    return tmpl;
}

TEST(GenerateFLE2MatchExpression, EmptyInput) {
    auto swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {});
    ASSERT(swExpr.isOK());
    ASSERT_BSONOBJ_EQ(mongo::fromjson("{$alwaysTrue: 1}"), swExpr.getValue()->serialize());
}

TEST(GenerateFLE2MatchExpression, SimpleInput) {
    EncryptedField foo(UUID::gen(), "foo", "string");
    EncryptedField bar(UUID::gen(), "bar", "string");

    std::string expectedJSON = R"({"$and":[
        {"$and":[
            <fooExpr>,
            <barExpr>
        ]}
    ]})";

    auto swExpr =
        generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {foo, bar});
    ASSERT(swExpr.isOK());
    auto outputBSON = swExpr.getValue()->serialize();

    replace_str(expectedJSON, "<fooExpr>", expectedLeafExpr(foo));
    replace_str(expectedJSON, "<barExpr>", expectedLeafExpr(bar));

    auto expectedBSON = mongo::fromjson(expectedJSON);
    ASSERT_BSONOBJ_EQ(expectedBSON, outputBSON);
}

TEST(GenerateFLE2MatchExpression, NormalInputWithNestedFields) {
    auto swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(),
                                                             kEncryptedFields);
    ASSERT(swExpr.isOK());

    auto outputBSON = swExpr.getValue()->serialize();

    std::string rootSchema = R"({"$and":[{"$and":[<aNonLeafExpr>, <cLeafExpr>]}]})";
    std::string aSubschema = R"({"$and":[<abNonLeafExpr>, <axNonLeafExpr>]})";
    std::string abSubschema = R"({"$and":[<abcLeafExpr>, <abdLeafExpr>]})";
    std::string axSubschema = R"({"$and":[<axyLeafExpr>]})";

    replace_str(rootSchema, "<cLeafExpr>", expectedLeafExpr(kFieldC));
    replace_str(rootSchema, "<aNonLeafExpr>", expectedNonLeafExpr("a", aSubschema));
    replace_all(rootSchema, "<abNonLeafExpr>", expectedNonLeafExpr("b", abSubschema));
    replace_all(rootSchema, "<axNonLeafExpr>", expectedNonLeafExpr("x", axSubschema));
    replace_all(rootSchema, "<abcLeafExpr>", expectedLeafExpr(kFieldAbc));
    replace_all(rootSchema, "<abdLeafExpr>", expectedLeafExpr(kFieldAbd));
    replace_all(rootSchema, "<axyLeafExpr>", expectedLeafExpr(kFieldAxy));

    auto expectedBSON = mongo::fromjson(rootSchema);
    ASSERT_BSONOBJ_EQ(expectedBSON, outputBSON);
}

DEATH_TEST(GenerateFLE2MatchExpression, EncryptedFieldsConflict, "tripwire assertions") {
    EncryptedField a(UUID::gen(), "a", "string");
    EncryptedField ab(UUID::gen(), "a.b", "int");
    EncryptedField abc(UUID::gen(), "a.b.c", "int");

    auto swExpr =
        generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {a, ab});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);

    swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {abc, ab});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);

    swExpr = generateMatchExpressionFromEncryptedFields(new ExpressionContextForTest(), {abc, abc});
    ASSERT(!swExpr.isOK());
    ASSERT(swExpr.getStatus().code() == 6364302);
}

class Fle2MatchTest : public unittest::Test {
public:
    Fle2MatchTest() {
        expr = uassertStatusOK(generateMatchExpressionFromEncryptedFields(
            new ExpressionContextForTest(), kEncryptedFields));
    }
    std::unique_ptr<MatchExpression> expr;
};

TEST_F(Fle2MatchTest, MatchesIfNoEncryptedFieldsInObject) {
    // no encrypted paths
    ASSERT(expr->matchesBSON(BSONObj()));
    ASSERT(expr->matchesBSON(fromjson(R"({name: "sue"})")));

    // has prefix of encrypted paths, but no leaf
    ASSERT(expr->matchesBSON(fromjson(R"({a: {}})")));
    ASSERT(expr->matchesBSON(fromjson(R"({a: {b: {}, x: { count: 23 }}})")));

    // non-object/non-array along the encrypted path
    ASSERT(expr->matchesBSON(fromjson(R"({a: 1}})")));
    ASSERT(expr->matchesBSON(fromjson(R"({a: { b: 2, x: "foo"}})")));
}

TEST_F(Fle2MatchTest, MatchesIfSomeEncryptedFieldsInObject) {
    auto obj = BSON("c" << makeFleBinData(kValueC) << "other"
                        << "foo");
    ASSERT(expr->matchesBSON(obj));

    obj = BSON("a" << BSON("b" << BSON("c" << makeFleBinData(kValueAbc))));
    ASSERT(expr->matchesBSON(obj));
}

TEST_F(Fle2MatchTest, MatchesIfAllEncryptedFieldsInObject) {
    auto allIn = BSON("c" << makeFleBinData(kValueC) << "a"
                          << BSON("b" << BSON("c" << makeFleBinData(kValueAbc) << "d"
                                                  << makeFleBinData(kValueAbd))
                                      << "x" << BSON("y" << makeFleBinData(kValueAxy))));
    ASSERT(expr->matchesBSON(allIn));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfEncryptedFieldIsNotBinDataEncrypt) {
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({a: {b: {c: "foo"}}})")));
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({c: []})")));
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({a: {x: {y: [1, 2, 3]}}})")));
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({a: {b: {d: 42}}})")));
    auto obj = BSON("c" << BSONBinData(nullptr, 0, BinDataType::BinDataGeneral));
    ASSERT_FALSE(expr->matchesBSON(obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfEncryptedFieldIsNotFLE2) {
    auto obj = BSON("c" << makeFleBinData(kValueFLE1));
    ASSERT_FALSE(expr->matchesBSON(obj));

    obj = BSON("a" << BSON("b" << BSON("c" << makeFleBinData(kValueFLE1))));
    ASSERT_FALSE(expr->matchesBSON(obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfTypeMismatch) {
    auto obj = BSON("c" << makeFleBinData(kValueAbc));
    ASSERT_FALSE(expr->matchesBSON(obj));

    obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON("c" << makeFleBinData(kValueAxy)))));
    ASSERT_FALSE(expr->matchesBSON(obj));
}

TEST_F(Fle2MatchTest, DoesNotMatchIfHasArrayInEncryptedFieldPath) {
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({a: []})")));
    ASSERT_FALSE(expr->matchesBSON(fromjson(R"({a: {b: [1, 2, 3]}})")));

    auto obj = BSON("a" << BSON_ARRAY(BSON("b" << BSON("c" << makeFleBinData(kValueAbc)))));
    ASSERT_FALSE(expr->matchesBSON(obj));
}

}  // namespace
}  // namespace mongo
