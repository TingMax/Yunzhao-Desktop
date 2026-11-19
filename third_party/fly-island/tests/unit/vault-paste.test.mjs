import assert from "node:assert/strict";
import test from "node:test";
import { parseLoginPaste } from "../../src/features/vault/paste.ts";

test("a copied account and password paste back as those two fields", () => {
  assert.deepEqual(parseLoginPaste("name@example.com\nsecret"), {
    purpose: "",
    account: "name@example.com",
    secret: "secret",
  });
});

test("labeled Chinese text fills the purpose, account and password", () => {
  assert.deepEqual(parseLoginPaste("用途：网盘\n账号：ada\n密码：p@ss word"), {
    purpose: "网盘",
    account: "ada",
    secret: "p@ss word",
  });
});

test("three plain lines are purpose, account and password", () => {
  assert.deepEqual(parseLoginPaste("网盘\nada\np@ss"), {
    purpose: "网盘",
    account: "ada",
    secret: "p@ss",
  });
});

test("a single unlabeled word is not an account", () => {
  assert.equal(parseLoginPaste("only-a-password"), null);
});
