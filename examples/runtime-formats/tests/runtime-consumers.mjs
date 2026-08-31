import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import {
  copyFileSync,
  cpSync,
  createReadStream,
  existsSync,
  mkdirSync,
  mkdtempSync,
  readFileSync,
  readdirSync,
  rmSync,
  writeFileSync,
} from "node:fs";
import http from "node:http";
import { basename, dirname, extname, join, resolve, sep } from "node:path";
import { fileURLToPath } from "node:url";
import { chromium } from "playwright";
import { PNG } from "pngjs";

const ROOT = resolve(dirname(fileURLToPath(import.meta.url)), "..");
const REPO = resolve(ROOT, "..", "..");
const ARTIFACTS = join(REPO, "build", "runtime-consumer-artifacts");
const MIME = new Map([
  [".css", "text/css; charset=utf-8"],
  [".html", "text/html; charset=utf-8"],
  [".js", "text/javascript; charset=utf-8"],
  [".json", "application/json"],
  [".png", "image/png"],
  [".wasm", "application/wasm"],
]);
const STAGE_FILES = [
  "defold/main.collection",
  "defold/main.script",
  "game.project",
  "index.html",
  "input/game.input_binding",
  "package.json",
  "runtime-formats.ntpacker_project",
  "sprites/coin.png",
  "sprites/hero.png",
  "src/main.js",
  "vite.config.js",
];

function argument(name, required = true) {
  const index = process.argv.indexOf(`--${name}`);
  const value = index >= 0 ? process.argv[index + 1] : undefined;
  if (required) assert(value, `--${name} is required`);
  return value;
}

function run(command, args, cwd) {
  const result = spawnSync(command, args, {
    cwd,
    encoding: "utf8",
    stdio: ["ignore", "pipe", "pipe"],
  });
  if (result.status !== 0) {
    throw new Error([
      `${basename(command)} failed with exit code ${result.status}`,
      result.stdout,
      result.stderr,
    ].filter(Boolean).join("\n"));
  }
  return result.stdout;
}

function stageProject() {
  const stage = mkdtempSync(join(ROOT, ".runtime-consumer-"));
  try {
    for (const file of STAGE_FILES) {
      const destination = join(stage, file);
      mkdirSync(dirname(destination), { recursive: true });
      copyFileSync(join(ROOT, file), destination);
    }
  } catch (error) {
    rmSync(stage, { recursive: true, force: true });
    throw error;
  }
  return stage;
}

function buildPhaser(stage) {
  const vite = join(ROOT, "node_modules", "vite", "bin", "vite.js");
  assert(existsSync(vite), "run npm ci before the runtime consumer test");
  run(process.execPath, [vite, "build", stage], ROOT);
  return join(stage, "dist");
}

function buildDefold(stage, java, bob) {
  assert(existsSync(java), `Java executable not found: ${java}`);
  assert(existsSync(bob), `Bob jar not found: ${bob}`);
  const bundle = join(stage, "defold-bundle");
  run(java, [
    "-jar", bob,
    "--archive",
    "--platform", "wasm-web",
    "--architectures", "wasm-web",
    "--variant", "debug",
    "--bundle-output", bundle,
    "resolve", "distclean", "build", "bundle",
  ], stage);
  const roots = readdirSync(bundle, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => join(bundle, entry.name))
    .filter((path) => existsSync(join(path, "index.html")));
  assert.equal(roots.length, 1, "Defold bundle must contain exactly one web root");
  return roots[0];
}

function startServer(root) {
  const server = http.createServer((request, response) => {
    const pathname = decodeURIComponent(new URL(request.url, "http://127.0.0.1").pathname);
    if (pathname === "/favicon.ico") {
      response.writeHead(204).end();
      return;
    }
    const relativePath = pathname === "/" ? "index.html" : pathname.slice(1);
    const file = resolve(root, relativePath);
    if (!file.startsWith(`${resolve(root)}${sep}`) || !existsSync(file)) {
      response.writeHead(404).end();
      return;
    }
    response.setHeader("Content-Type", MIME.get(extname(file)) ?? "application/octet-stream");
    createReadStream(file).pipe(response);
  });
  return new Promise((resolveReady) => {
    server.listen(0, "127.0.0.1", () => resolveReady({
      server,
      url: `http://127.0.0.1:${server.address().port}/`,
    }));
  });
}

function exactPixels(name, expectedBytes, actualBytes) {
  const expected = PNG.sync.read(expectedBytes);
  const actual = PNG.sync.read(actualBytes);
  assert.equal(actual.width, expected.width, `${name}: screenshot width changed`);
  assert.equal(actual.height, expected.height, `${name}: screenshot height changed`);
  const diff = new PNG({ width: expected.width, height: expected.height });
  let changed = 0;
  for (let offset = 0; offset < expected.data.length; offset += 4) {
    const same = expected.data.subarray(offset, offset + 4)
      .equals(actual.data.subarray(offset, offset + 4));
    if (!same) ++changed;
    const target = same ? actual.data.subarray(offset, offset + 4) : Buffer.from([255, 0, 255, 255]);
    target.copy(diff.data, offset);
  }
  if (changed !== 0) {
    writeFileSync(join(ARTIFACTS, `${name}-diff.png`), PNG.sync.write(diff));
    assert.fail(`${name}: ${changed} pixels differ from tests/expected.png`);
  }
}

async function capture(browser, name, root) {
  const { server, url } = await startServer(root);
  try {
    const failures = [];
    const page = await browser.newPage({
      viewport: { width: 256, height: 144 },
      deviceScaleFactor: 1,
    });
    try {
      page.on("pageerror", (error) => failures.push(`pageerror: ${error.message}`));
      page.on("requestfailed", (request) => failures.push(
        `requestfailed: ${request.url()} ${request.failure()?.errorText ?? ""}`));
      page.on("response", (response) => {
        if (response.status() >= 400) failures.push(`http ${response.status()}: ${response.url()}`);
      });
      page.on("console", (message) => {
        const content = message.text();
        if (message.type() === "error" || /(^|\s)(ERROR|FATAL)(:|\s|$)/i.test(content)) {
          failures.push(`console.${message.type()}: ${content}`);
        }
      });
      await page.goto(url, { waitUntil: "networkidle" });
      await page.waitForFunction(
        (engine) => window.__NTPACKER_TEST__?.ready === true
          && window.__NTPACKER_TEST__?.engine === engine,
        name,
        { timeout: 30_000 },
      );
      await page.evaluate(() => new Promise((done) =>
        requestAnimationFrame(() => requestAnimationFrame(done))));
      const canvas = page.locator("canvas");
      const first = await canvas.screenshot();
      writeFileSync(join(ARTIFACTS, `${name}-first.png`), first);
      await page.evaluate(() => new Promise((done) =>
        requestAnimationFrame(() => requestAnimationFrame(done))));
      const second = await canvas.screenshot();
      copyFileSync(join(ROOT, "tests", "expected.png"), join(ARTIFACTS, "expected.png"));
      writeFileSync(join(ARTIFACTS, `${name}-actual.png`), second);
      exactPixels(`${name}-stability`, first, second);
      exactPixels(name, readFileSync(join(ROOT, "tests", "expected.png")), second);
      assert.deepEqual(failures, [], `${name}: browser failures`);
    } catch (error) {
      writeFileSync(join(ARTIFACTS, `${name}-failure.txt`), [
        error.stack ?? String(error),
        ...failures,
      ].join("\n"));
      const canvas = page.locator("canvas");
      if (await canvas.count() === 1) {
        writeFileSync(join(ARTIFACTS, `${name}-failure.png`), await canvas.screenshot());
      }
      throw error;
    } finally {
      await page.close();
    }
  } finally {
    await new Promise((resolveClose) => server.close(resolveClose));
  }
}

const ntpacker = resolve(argument("ntpacker"));
const engines = (argument("engines", false) ?? "phaser,defold").split(",");
const java = argument("java", engines.includes("defold"));
const bob = argument("bob", engines.includes("defold"));
assert(existsSync(ntpacker), `ntpacker executable not found: ${ntpacker}`);
rmSync(ARTIFACTS, { recursive: true, force: true });
mkdirSync(ARTIFACTS, { recursive: true });
let stage;
let browser;
try {
  stage = stageProject();
  run(ntpacker, ["pack", join(stage, "runtime-formats.ntpacker_project"), "--json"], stage);
  const roots = [];
  for (const engine of engines) {
    switch (engine) {
      case "phaser":
        roots.push([engine, buildPhaser(stage)]);
        break;
      case "defold":
        roots.push([engine, buildDefold(stage, resolve(java), resolve(bob))]);
        break;
      default:
        throw new Error(`unknown engine: ${engine}`);
    }
  }
  browser = await chromium.launch({
    headless: true,
    args: ["--use-angle=swiftshader", "--force-device-scale-factor=1"],
  });
  for (const [name, root] of roots) await capture(browser, name, root);
} catch (error) {
  writeFileSync(join(ARTIFACTS, "setup-failure.txt"), error.stack ?? String(error));
  if (stage && existsSync(join(stage, "golden"))) {
    cpSync(join(stage, "golden"), join(ARTIFACTS, "staged-golden"), { recursive: true });
  }
  throw error;
} finally {
  if (browser) await browser.close();
  if (stage) rmSync(stage, { recursive: true, force: true });
}
