/*
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include <string>

#include "util/parson.h"

/* ------------------------------------------------------------------ */
/* Missing-file NULL returns                                           */
/* ------------------------------------------------------------------ */

class ParsonMissingFileTest : public ::testing::Test {
protected:
   void SetUp() override
   {
      const testing::internal::FilePath temp_dir(::testing::TempDir());
      const testing::internal::FilePath missing_file =
         testing::internal::FilePath::GenerateUniqueFileName(
            temp_dir, testing::internal::FilePath("parson_test_missing"),
            "json");

      ASSERT_FALSE(missing_file.FileOrDirectoryExists());
      missing_path = missing_file.string();
   }

   const char *
   MissingPath() const
   {
      return missing_path.c_str();
   }

private:
   std::string missing_path;
};

TEST_F(ParsonMissingFileTest, parse_file_nonexistent_returns_null)
{
   JSON_Value *val = json_parse_file(MissingPath());
   EXPECT_EQ(val, nullptr);
}

TEST_F(ParsonMissingFileTest, parse_file_with_comments_nonexistent_returns_null)
{
   JSON_Value *val = json_parse_file_with_comments(MissingPath());
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
