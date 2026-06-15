/*
 * Copyright (c) 2026 Terascale Functionalists
 * SPDX-License-Identifier: MIT
 */

#include <gtest/gtest.h>

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "util/parson.h"

/* ------------------------------------------------------------------ */
/* Missing-file NULL returns                                           */
/* ------------------------------------------------------------------ */

class ParsonMissingFileTest : public ::testing::Test {
protected:
   void SetUp() override
   {
      std::string template_path = ::testing::TempDir();
      if (template_path.empty() || template_path.back() != '/')
         template_path += '/';
      template_path += "parson_test.XXXXXX";

      temp_dir_template.assign(template_path.begin(), template_path.end());
      temp_dir_template.push_back('\0');
      char *created_dir = mkdtemp(temp_dir_template.data());
      ASSERT_NE(created_dir, nullptr);

      temp_dir = created_dir;
      missing_path = temp_dir + "/missing.json";

      errno = 0;
      ASSERT_EQ(access(missing_path.c_str(), F_OK), -1);
      ASSERT_EQ(errno, ENOENT);
   }

   void TearDown() override
   {
      if (!temp_dir.empty())
         EXPECT_EQ(rmdir(temp_dir.c_str()), 0);
   }

   const char *
   MissingPath() const
   {
      return missing_path.c_str();
   }

private:
   std::vector<char> temp_dir_template;
   std::string temp_dir;
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
