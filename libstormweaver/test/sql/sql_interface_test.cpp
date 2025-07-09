#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <iostream>

#include "sql.hpp"
#include "sql_variant/generic.hpp"

using namespace sql_variant;

class SqlCleanupFixture {
public:
  SqlCleanupFixture() {
    // Recreate public schema to ensure clean state
    sqlConnection->executeQuery("DROP SCHEMA IF EXISTS public CASCADE")
        .maybeThrow();
    sqlConnection->executeQuery("CREATE SCHEMA public").maybeThrow();
    sqlConnection->executeQuery("GRANT ALL ON SCHEMA public TO public")
        .maybeThrow();
  }
};

TEST_CASE_METHOD(SqlCleanupFixture, "SQL Interface - Name-based field access",
                 "[sql_interface][integration]") {
  // Create a test table with known column names using a unique name
  const std::string createTableSql = R"(
    CREATE TABLE test_users (
      id SERIAL PRIMARY KEY,
      user_name VARCHAR(50) NOT NULL,
      email_address VARCHAR(100),
      age INTEGER,
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )
  )";

  auto createResult = sqlConnection->executeQuery(createTableSql);
  REQUIRE(createResult.success());

  // Insert test data
  const std::string insertSql = R"(
    INSERT INTO test_users (user_name, email_address, age) VALUES
    ('Alice Johnson', 'alice@example.com', 30),
    ('Bob Smith', 'bob@example.com', 25),
    ('Charlie Brown', NULL, 35)
  )";

  auto insertResult = sqlConnection->executeQuery(insertSql);
  REQUIRE(insertResult.success());

  SECTION("Query with explicit column names") {
    const std::string querySql =
        "SELECT id, user_name, email_address, age FROM "
        "test_users ORDER BY id";
    auto queryResult = sqlConnection->executeQuery(querySql);

    REQUIRE(queryResult.success());
    REQUIRE(queryResult.data != nullptr);
    REQUIRE(queryResult.data->numRows() == 3);
    REQUIRE(queryResult.data->numFields() == 4);

    // Test column metadata
    auto fieldNames = queryResult.data->fieldNames();
    REQUIRE(fieldNames.size() == 4);
    REQUIRE(fieldNames[0] == "id");
    REQUIRE(fieldNames[1] == "user_name");
    REQUIRE(fieldNames[2] == "email_address");
    REQUIRE(fieldNames[3] == "age");

    // Test field index lookup
    REQUIRE(queryResult.data->fieldIndex("id") == 0);
    REQUIRE(queryResult.data->fieldIndex("user_name") == 1);
    REQUIRE(queryResult.data->fieldIndex("email_address") == 2);
    REQUIRE(queryResult.data->fieldIndex("age") == 3);
    REQUIRE(queryResult.data->fieldIndex("invalid_column") == std::nullopt);

    // Test field name lookup
    REQUIRE(queryResult.data->fieldName(0) == "id");
    REQUIRE(queryResult.data->fieldName(1) == "user_name");
    REQUIRE(queryResult.data->fieldName(2) == "email_address");
    REQUIRE(queryResult.data->fieldName(3) == "age");
    REQUIRE(queryResult.data->fieldName(4) == "");

    // Test first row - both access methods
    auto row1 = queryResult.data->nextRow();

    // Index-based access (existing functionality)
    REQUIRE(row1.field(0).has_value());
    REQUIRE(row1.field(1) == "Alice Johnson");
    REQUIRE(row1.field(2) == "alice@example.com");
    REQUIRE(row1.field(3) == "30");

    // Name-based access (new functionality)
    REQUIRE(row1.field("id").has_value());
    REQUIRE(row1.field("user_name") == "Alice Johnson");
    REQUIRE(row1.field("email_address") == "alice@example.com");
    REQUIRE(row1.field("age") == "30");

    // Test field existence
    REQUIRE(row1.hasField("id") == true);
    REQUIRE(row1.hasField("user_name") == true);
    REQUIRE(row1.hasField("email_address") == true);
    REQUIRE(row1.hasField("age") == true);
    REQUIRE(row1.hasField("invalid_column") == false);

    // Test row field names
    auto rowFieldNames = row1.fieldNames();
    REQUIRE(rowFieldNames.size() == 4);

    // Test second row
    auto row2 = queryResult.data->nextRow();
    REQUIRE(row2.field("user_name") == "Bob Smith");
    REQUIRE(row2.field("email_address") == "bob@example.com");
    REQUIRE(row2.field("age") == "25");

    // Test third row with NULL value
    auto row3 = queryResult.data->nextRow();
    REQUIRE(row3.field("user_name") == "Charlie Brown");
    REQUIRE(row3.field("email_address") == std::nullopt);
    REQUIRE(row3.field("age") == "35");
  }

  SECTION("Query with column aliases") {
    const std::string querySql = R"(
      SELECT 
        id AS user_id,
        user_name AS full_name,
        email_address AS email,
        age AS user_age
      FROM test_users 
      ORDER BY id
      LIMIT 1
    )";

    auto queryResult = sqlConnection->executeQuery(querySql);
    REQUIRE(queryResult.success());
    REQUIRE(queryResult.data != nullptr);
    REQUIRE(queryResult.data->numRows() == 1);
    REQUIRE(queryResult.data->numFields() == 4);

    // Test aliased column names
    auto fieldNames = queryResult.data->fieldNames();
    REQUIRE(fieldNames[0] == "user_id");
    REQUIRE(fieldNames[1] == "full_name");
    REQUIRE(fieldNames[2] == "email");
    REQUIRE(fieldNames[3] == "user_age");

    auto row = queryResult.data->nextRow();

    // Access by alias names
    REQUIRE(row.field("user_id").has_value());
    REQUIRE(row.field("full_name") == "Alice Johnson");
    REQUIRE(row.field("email") == "alice@example.com");
    REQUIRE(row.field("user_age") == "30");

    // Original column names should not work
    REQUIRE(row.field("id") == std::nullopt);
    REQUIRE(row.field("user_name") == std::nullopt);
    REQUIRE(row.field("email_address") == std::nullopt);
    REQUIRE(row.field("age") == std::nullopt);
  }

  SECTION("Empty result set") {
    const std::string querySql =
        "SELECT id, user_name FROM test_users WHERE id = -1";
    auto queryResult = sqlConnection->executeQuery(querySql);

    REQUIRE(queryResult.success());
    REQUIRE(queryResult.data != nullptr);
    REQUIRE(queryResult.data->numRows() == 0);
    REQUIRE(queryResult.data->numFields() == 2);

    // Column metadata should still be available
    auto fieldNames = queryResult.data->fieldNames();
    REQUIRE(fieldNames.size() == 2);
    REQUIRE(fieldNames[0] == "id");
    REQUIRE(fieldNames[1] == "user_name");
  }

  SECTION("Case sensitivity test") {
    const std::string querySql = R"(
      SELECT 
        user_name AS "UserName",
        email_address AS "EMAIL_ADDRESS"
      FROM test_users 
      LIMIT 1
    )";

    auto queryResult = sqlConnection->executeQuery(querySql);
    REQUIRE(queryResult.success());
    REQUIRE(queryResult.data != nullptr);

    auto row = queryResult.data->nextRow();

    REQUIRE(row.field("UserName") == "Alice Johnson");
    REQUIRE(row.field("EMAIL_ADDRESS") == "alice@example.com");

    REQUIRE(row.field("username") == std::nullopt);
    REQUIRE(row.field("email_address") == std::nullopt);
  }

  SECTION("Special characters in column names") {
    const std::string querySql = R"(
      SELECT 
        user_name AS "user name",
        email_address AS "email-address",
        age AS "user.age"
      FROM test_users 
      LIMIT 1
    )";

    auto queryResult = sqlConnection->executeQuery(querySql);
    REQUIRE(queryResult.success());
    REQUIRE(queryResult.data != nullptr);

    auto row = queryResult.data->nextRow();

    REQUIRE(row.field("user name") == "Alice Johnson");
    REQUIRE(row.field("email-address") == "alice@example.com");
    REQUIRE(row.field("user.age") == "30");
  }
}

TEST_CASE_METHOD(SqlCleanupFixture, "SQL Interface - INSERT with RETURNING",
                 "[sql_interface][integration]") {
  const std::string createTableSql = R"(
    CREATE TABLE test_products (
      id SERIAL PRIMARY KEY,
      name VARCHAR(100) NOT NULL,
      price DECIMAL(10,2),
      created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
    )
  )";

  auto createResult = sqlConnection->executeQuery(createTableSql);
  REQUIRE(createResult.success());

  SECTION("INSERT with RETURNING clause") {
    const std::string insertSql = R"(
      INSERT INTO test_products (name, price) 
      VALUES ('Test Product', 29.99) 
      RETURNING id, name, price
    )";

    auto insertResult = sqlConnection->executeQuery(insertSql);
    REQUIRE(insertResult.success());
    REQUIRE(insertResult.data != nullptr);
    REQUIRE(insertResult.data->numRows() == 1);
    REQUIRE(insertResult.data->numFields() == 3);

    auto row = insertResult.data->nextRow();
    REQUIRE(row.field("id").has_value());
    REQUIRE(row.field("name") == "Test Product");
    REQUIRE(row.field("price") == "29.99");

    REQUIRE(row.field(0).has_value());
    REQUIRE(row.field(1) == "Test Product");
    REQUIRE(row.field(2) == "29.99");
  }
}