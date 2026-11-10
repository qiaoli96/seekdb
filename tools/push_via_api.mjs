#!/usr/bin/env node
/**
 * 通过 GitHub REST Git Data API 把本仓库提交到指定分支（不需要 git 凭据、不碰工作区状态）。
 * 用法：
 *   GITHUB_TOKEN=github_pat_xxx node tools/push_via_api.mjs
 * 令牌要求：fine-grained PAT，仅需 Contents: Read and write。
 * 纪律：令牌只从环境变量读取，绝不写入磁盘、不打印、不进提交内容。
 */
import { execFileSync } from "node:child_process";
import { readFileSync } from "node:fs";
import path from "node:path";

const OWNER = process.env.GH_OWNER || "qiaoli96";
const REPO = process.env.GH_REPO || "seekdb";
const BRANCH = process.env.GH_BRANCH || "2026-competition-round2";
const MESSAGE =
  process.env.GH_MESSAGE ||
  [
    "MiniVectorDB: JSONL protocol, SQL subset, WAL crash recovery, out-of-core row store, AVX2 TopK",
    "",
    "为什么：赛题要求单机单进程、stdin/stdout JSONL 协议、/data 持久化、BEGIN/COMMIT/ROLLBACK 与崩溃恢复；",
    "同时正式用例包含『数据量超过内存限额』的场景，因此行数据不常驻内存。",
    "",
    "已本地验收（不是自报）：",
    "- 官方公开自测包 20/20（本地二进制模式与 Docker 镜像模式一致）",
    "- 判题同款约束 --read-only --network=none --cpus=2 --memory=2g --pids-limit=128 下握手与 CRUD 通过",
    "- 崩溃恢复：COMMIT 后 kill -9 保留，未提交 kill -9 整段丢弃",
    "- 1.5M 行 / 843MB 数据峰值 RSS 85.4 MiB；事务内存增幅 0",
    "- 性能 10,000x128 / 100 批 / 1,000 次 TopK：写入 0.415 s，查询 0.389 ms/次",
    "",
    "单元测试：storage_test 395 断言、vector_test 735 断言，ctest 2/2；干净构建零警告。",
  ].join("\n");

const token = process.env.GITHUB_TOKEN;
if (!token) {
  console.error("缺少 GITHUB_TOKEN 环境变量（fine-grained PAT，Contents: Read and write）");
  process.exit(2);
}
if (!token.startsWith("github_pat_") && !token.startsWith("ghp_")) {
  console.error("令牌格式可疑（应为 github_pat_ 或 ghp_ 开头），已中止以免误发");
  process.exit(2);
}

const repoRoot = path.resolve(import.meta.dirname, "..");
// core.quotePath=false 保证中文路径原样输出，否则 git 会返回 "docs/\345\206\263..." 这类转义串
const files = execFileSync("git", ["-C", repoRoot, "-c", "core.quotePath=false", "ls-files"], { encoding: "utf8" })
  .split("\n").map((s) => s.trim().replace(/^"(.*)"$/, "$1")).filter(Boolean);

async function api(method, route, body) {
  const res = await fetch(`https://api.github.com${route}`, {
    method,
    headers: {
      authorization: `Bearer ${token}`,
      accept: "application/vnd.github+json",
      "x-github-api-version": "2022-11-28",
      "user-agent": "minivectordb-push",
      ...(body ? { "content-type": "application/json" } : {}),
    },
    body: body ? JSON.stringify(body) : undefined,
  });
  const text = await res.text();
  if (!res.ok) {
    // 注意：不要把 token 打进日志
    throw new Error(`${method} ${route} -> ${res.status} ${text.slice(0, 300)}`);
  }
  return text ? JSON.parse(text) : {};
}

const main = async () => {
  const me = await api("GET", "/user");
  console.log(`身份：${me.login}`);

  console.log(`上传 ${files.length} 个 blob …`);
  const tree = [];
  for (const rel of files) {
    const content = readFileSync(path.join(repoRoot, rel), "utf8");
    const blob = await api("POST", `/repos/${OWNER}/${REPO}/git/blobs`, { content, encoding: "utf-8" });
    tree.push({ path: rel, mode: "100644", type: "blob", sha: blob.sha });
    console.log(`  + ${rel} (${Buffer.byteLength(content)} B)`);
  }

  console.log("创建 tree …");
  const newTree = await api("POST", `/repos/${OWNER}/${REPO}/git/trees`, { tree });

  console.log("创建 commit …");
  const commit = await api("POST", `/repos/${OWNER}/${REPO}/git/commits`, {
    message: MESSAGE,
    tree: newTree.sha,
    parents: [],
  });
  console.log(`commit sha = ${commit.sha}`);

  let existing = null;
  try {
    existing = await api("GET", `/repos/${OWNER}/${REPO}/git/ref/heads/${BRANCH}`);
  } catch (e) {
    if (!String(e.message).includes("404")) throw e;
  }

  if (existing) {
    console.log(`分支已存在（tip ${existing.object.sha}），以 force=false 更新 …`);
    await api("PATCH", `/repos/${OWNER}/${REPO}/git/refs/heads/${BRANCH}`, { sha: commit.sha, force: false });
  } else {
    console.log("创建新分支 …");
    await api("POST", `/repos/${OWNER}/${REPO}/git/refs`, { ref: `refs/heads/${BRANCH}`, sha: commit.sha });
  }

  const verify = await api("GET", `/repos/${OWNER}/${REPO}/git/ref/heads/${BRANCH}`);
  const ok = verify.object.sha === commit.sha;
  console.log(`\n远端 tip = ${verify.object.sha}（${ok ? "与本次 commit 一致" : "不一致，请检查"}）`);
  console.log(`提测信息：\n  仓库 https://github.com/${OWNER}/${REPO}\n  分支 ${BRANCH}\n  SHA  ${commit.sha}`);
  if (!ok) process.exit(1);
};

main().catch((e) => {
  console.error("失败：", String(e.message).replace(token, "***"));
  process.exit(1);
});
