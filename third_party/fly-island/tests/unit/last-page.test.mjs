import assert from "node:assert/strict";
import test from "node:test";
import { normalizeLastPage } from "../../src/windows/island/lastPage.ts";

test("the remembered page must be one the island keeps; anything else falls back to Music", () => {
  for (const page of ["music", "vault", "qq", "agents", "translation"]) {
    assert.equal(normalizeLastPage(page), page);
  }
  for (const bad of ["todo", "clipboard", "shelf", "settings", "", null, 1, "Music", {}]) {
    assert.equal(normalizeLastPage(bad), "music");
  }
});
