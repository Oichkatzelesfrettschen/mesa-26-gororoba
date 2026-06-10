/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include "util/parson.h"

/* ------------------------------------------------------------------ */
/* Missing-file NULL returns                                           */
/* ------------------------------------------------------------------ */

TEST(parson, parse_file_nonexistent_returns_null)
{
   JSON_Value *val = json_parse_file("/nonexistent/path/x.json");
   EXPECT_EQ(val, nullptr);
}

TEST(parson, parse_file_with_comments_nonexistent_returns_null)
{
   JSON_Value *val = json_parse_file_with_comments("/nonexistent/path/x.json");
   EXPECT_EQ(val, nullptr);
}

/* ------------------------------------------------------------------ */
/* Linkage positive control                                            */
/* ------------------------------------------------------------------ */

TEST(parson, parse_string_valid_object)
{
   /* Verify that parson is linked and can parse a trivial JSON object. */
   JSON_Value *val = json_parse_string("{\"key\": 42}");
   ASSERT_NE(val, nullptr);
   EXPECT_EQ(json_value_get_type(val), JSONObject);
   json_value_free(val);
}

TEST(parson, parse_string_invalid_returns_null)
{
   JSON_Value *val = json_parse_string("not json at all {{{{");
   EXPECT_EQ(val, nullptr);
}
