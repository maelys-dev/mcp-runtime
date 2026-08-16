#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
  echo "usage: $0 <candidate-commit-sha>" >&2
  exit 2
fi

candidate=$1
if [[ ! $candidate =~ ^[0-9a-f]{40}$ ]]; then
  echo "workflow boundary: candidate must be a full lowercase commit SHA" >&2
  exit 2
fi
git cat-file -e "${candidate}^{commit}"

ruby -rpsych -ropen3 - "$candidate" <<'RUBY'
candidate = ARGV.fetch(0)

def fail!(message)
  abort("workflow boundary: #{message}")
end

def capture_git!(*args)
  stdout, stderr, status = Open3.capture3("git", *args)
  fail!("git #{args.join(' ')} failed: #{stderr.strip}") unless status.success?
  stdout
end

def decode(node)
  case node
  when Psych::Nodes::Scalar
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    node.value
  when Psych::Nodes::Sequence
    fail!("flow sequences are not allowed") unless node.style == Psych::Nodes::Sequence::BLOCK
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    node.children.map { |child| decode(child) }
  when Psych::Nodes::Mapping
    fail!("flow mappings are not allowed") unless node.style == Psych::Nodes::Mapping::BLOCK
    fail!("anchors and tags are not allowed") if node.anchor || node.tag
    fail!("mapping has an odd number of nodes") unless node.children.length.even?
    value = {}
    node.children.each_slice(2) do |key, child|
      fail!("mapping keys must be plain scalars") unless key.is_a?(Psych::Nodes::Scalar)
      decoded_key = decode(key)
      fail!("duplicate mapping key #{decoded_key.inspect}") if value.key?(decoded_key)
      value[decoded_key] = decode(child)
    end
    value
  when Psych::Nodes::Alias
    fail!("aliases are not allowed")
  else
    fail!("unexpected YAML node #{node.class}")
  end
end

def load_candidate_yaml(candidate, path)
  content = capture_git!("show", "#{candidate}:#{path}")
  stream = Psych.parse_stream(content)
  fail!("#{path} must contain exactly one YAML document") unless stream.children.length == 1
  document = stream.children.fetch(0)
  fail!("#{path} document anchors and tags are not allowed") if document.tag || document.implicit == false
  decode(document.root)
end

def require_value!(actual, expected, label)
  fail!("#{label} does not match the sealed contract") unless actual == expected
end

def regular_blob_content!(ref, path)
  entry = capture_git!("ls-tree", ref, "--", path).strip
  metadata, actual_path = entry.split("\t", 2)
  mode, type, = metadata.to_s.split(" ", 3)
  fail!("#{path} must be a regular blob in #{ref}") unless
    ["100644", "100755"].include?(mode) && type == "blob" && actual_path == path
  capture_git!("show", "#{ref}:#{path}")
end

def normalize_makefile_version!(content, label)
  matches = content.scan(/^VERSION := [^\n]+$/)
  fail!("#{label} must contain exactly one VERSION := line") unless matches.length == 1
  content.sub(/^VERSION := [^\n]+$/, "VERSION := <release-version>")
end

def validate_release_make_target!(content)
  recipe_blocks = []
  lines = content.lines
  lines.each_with_index do |line, index|
    next unless line == "test-release-contract:\n"

    recipes = []
    cursor = index + 1
    while cursor < lines.length && lines[cursor].start_with?("\t")
      recipes << lines[cursor]
      cursor += 1
    end
    recipe_blocks << recipes
  end
  fail!("Makefile must define test-release-contract exactly once") unless recipe_blocks.length == 1
  require_value!(
    recipe_blocks.fetch(0),
    [
      "\tbash scripts/check-release-contract.sh\n",
      "\tbash scripts/check-release-workflow.sh\n",
      "\tbash scripts/test-release-transaction.sh\n"
    ],
    "test-release-contract recipe"
  )
end

expected_paths = [
  ".github/workflows/ci.yml",
  ".github/workflows/release.yml",
  ".github/workflows/workflow-boundary.yml"
]
entries = capture_git!("ls-tree", "-r", "-z", candidate, "--", ".github/workflows").split("\0").reject(&:empty?)
actual_paths = []
entries.each do |entry|
  metadata, path = entry.split("\t", 2)
  mode, type, = metadata.split(" ", 3)
  fail!("malformed workflow tree entry") unless path && mode && type
  fail!("#{path} must be a regular blob") unless mode == "100644" && type == "blob"
  actual_paths << path
end
require_value!(actual_paths.sort, expected_paths, "candidate workflow paths")

checkout = "actions/checkout@fbc6f3992d24b796d5a048ff273f7fcc4a7b6c09"
setup_node = "actions/setup-node@a0853c24544627f65ddf259abe73b1d18a591444"
install_with_node = "sudo apt-get update && sudo apt-get install --yes clang make pkg-config libjansson-dev liburiparser-dev python3 ripgrep ruby"
install_native = "sudo apt-get update && sudo apt-get install --yes clang make pkg-config libjansson-dev liburiparser-dev"
install_conformance = "sudo apt-get update && sudo apt-get install --yes clang make pkg-config libjansson-dev liburiparser-dev python3"

expected_ci = {
  "name" => "CI",
  "on" => {
    "push" => { "branches" => ["**"] },
    "pull_request" => "",
    "workflow_dispatch" => ""
  },
  "permissions" => { "contents" => "read" },
  "concurrency" => {
    "group" => "mcp-runtime-ci-${{ github.workflow }}-${{ github.ref }}",
    "cancel-in-progress" => "${{ github.event_name == 'pull_request' }}"
  },
  "env" => { "DEBIAN_FRONTEND" => "noninteractive" },
  "jobs" => {
    "check" => {
      "name" => "Native, SDK and provider checks",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "15",
      "steps" => [
        { "uses" => checkout },
        { "uses" => setup_node, "with" => { "node-version" => "24", "package-manager-cache" => "false" } },
        { "name" => "Install native dependencies", "run" => install_with_node },
        { "run" => "make check-all CC=clang ANALYZER=clang" },
        { "run" => "make test-release-contract" },
        { "run" => "make analyze CC=clang ANALYZER=clang" }
      ]
    },
    "sanitizers" => {
      "name" => "ASan, UBSan and TSan",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "20",
      "steps" => [
        { "uses" => checkout },
        { "name" => "Install native dependencies", "run" => install_native },
        { "run" => "make asan CC=clang" },
        { "run" => "make tsan CC=clang" }
      ]
    },
    "fuzz" => {
      "name" => "Fuzzer smoke campaigns",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "15",
      "steps" => [
        { "uses" => checkout },
        { "name" => "Install native dependencies", "run" => install_native },
        { "run" => "make fuzz-smoke CC=clang FUZZ_CC=clang" }
      ]
    },
    "official-conformance" => {
      "name" => "Supported official MCP scenarios",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "15",
      "steps" => [
        { "uses" => checkout },
        { "uses" => setup_node, "with" => { "node-version" => "24", "package-manager-cache" => "false" } },
        { "name" => "Install native dependencies", "run" => install_conformance },
        { "run" => "make test-mcp-conformance-official CC=clang" }
      ]
    }
  }
}

tag_validation = <<~SH
  test "$GITHUB_REF_TYPE" = "tag"
  version="$(bash scripts/release-version.sh)"
  test "$GITHUB_REF_NAME" = "v$version"
  bash scripts/check-release-contract.sh
SH
publish_release = <<~SH
  version="$(bash scripts/release-version.sh)"
  gh release create "$GITHUB_REF_NAME" --title "mcp-runtime $version" --generate-notes --verify-tag
SH
expected_release = {
  "name" => "Release",
  "on" => { "push" => { "tags" => ["v*"] } },
  "permissions" => { "contents" => "read" },
  "jobs" => {
    "verify" => {
      "name" => "Verify tagged release",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "20",
      "steps" => [
        { "uses" => checkout },
        { "uses" => setup_node, "with" => { "node-version" => "24", "package-manager-cache" => "false" } },
        { "name" => "Install native dependencies", "run" => install_conformance },
        { "name" => "Verify tag and release metadata", "run" => tag_validation },
        { "run" => "make check-all CC=clang ANALYZER=clang" }
      ]
    },
    "publish" => {
      "name" => "Publish GitHub release",
      "needs" => "verify",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "5",
      "permissions" => { "contents" => "write" },
      "steps" => [
        { "uses" => checkout },
        {
          "name" => "Create GitHub release for the pushed tag",
          "env" => { "GH_TOKEN" => "${{ github.token }}" },
          "run" => publish_release
        }
      ]
    }
  }
}

fetch_candidate = <<~SH
  [[ "$CANDIDATE_SHA" =~ ^[0-9a-f]{40}$ ]]
  [[ "$PR_NUMBER" =~ ^[1-9][0-9]*$ ]]
  git fetch --no-tags origin "refs/pull/${PR_NUMBER}/head:refs/mcp-runtime/candidate"
  test "$(git rev-parse refs/mcp-runtime/candidate)" = "$CANDIDATE_SHA"
SH
expected_boundary = {
  "name" => "Workflow boundary",
  "on" => { "pull_request_target" => { "types" => ["opened", "reopened", "synchronize"] } },
  "permissions" => { "contents" => "read" },
  "jobs" => {
    "workflow-boundary" => {
      "name" => "Workflow boundary",
      "runs-on" => "ubuntu-24.04",
      "timeout-minutes" => "5",
      "steps" => [
        {
          "uses" => checkout,
          "with" => {
            "ref" => "${{ github.event.repository.default_branch }}",
            "persist-credentials" => "false"
          }
        },
        {
          "name" => "Fetch candidate workflow objects",
          "env" => {
            "CANDIDATE_SHA" => "${{ github.event.pull_request.head.sha }}",
            "PR_NUMBER" => "${{ github.event.pull_request.number }}"
          },
          "run" => fetch_candidate
        },
        {
          "name" => "Validate candidate workflow boundary",
          "env" => { "CANDIDATE_SHA" => "${{ github.event.pull_request.head.sha }}" },
          "run" => "bash scripts/check-workflow-boundary.sh \"$CANDIDATE_SHA\""
        }
      ]
    }
  }
}

require_value!(load_candidate_yaml(candidate, ".github/workflows/ci.yml"), expected_ci, "ci.yml")
require_value!(load_candidate_yaml(candidate, ".github/workflows/release.yml"), expected_release, "release.yml")
require_value!(load_candidate_yaml(candidate, ".github/workflows/workflow-boundary.yml"), expected_boundary, "workflow-boundary.yml")

sealed_paths = [
  "scripts/check-workflow-boundary.sh",
  "scripts/check-release-contract.sh",
  "scripts/check-release-workflow.sh",
  "scripts/check-release-binding.sh",
  "scripts/check-release-secrets.sh",
  "scripts/release-version.sh",
  "scripts/prepare-release.sh"
]
sealed_paths.each do |path|
  require_value!(
    regular_blob_content!(candidate, path),
    regular_blob_content!("HEAD", path),
    path
  )
end

candidate_makefile = regular_blob_content!(candidate, "Makefile")
base_makefile = regular_blob_content!("HEAD", "Makefile")
validate_release_make_target!(candidate_makefile)
require_value!(
  normalize_makefile_version!(candidate_makefile, "candidate Makefile"),
  normalize_makefile_version!(base_makefile, "base Makefile"),
  "Makefile outside VERSION"
)

puts "workflow boundary: PASS"
RUBY
