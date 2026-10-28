#!/usr/bin/env python3
"""Lint the GitHub Actions workflows before pushing them.

GitHub only validates a workflow *after* it is pushed, and a broken file means
no job runs at all - which is impossible to notice from CI, because CI is the
thing that is broken. This script reproduces the three checks that matter here:

  1. YAML parses (with GitHub's scalar rules, so `on:` is not a boolean).
  2. The document validates against SchemaStore's github-workflow schema
     (skipped when the schema file is not available).
  3. Every expression is well formed - including bare expressions in `if:`,
     where GitHub's grammar rejects property names that start with a digit
     (`steps.plan.outputs.3dsx` is invalid, `steps.plan.outputs.want_3dsx` is
     fine). Also checks that `steps.<id>` is defined earlier in the job, and
     runs `bash -n` on every `run:` block.

Usage:
    python3 tools/lint-workflow.py                      # all workflows
    python3 tools/lint-workflow.py .github/workflows/build.yml
    python3 tools/lint-workflow.py --schema gh-workflow.json

Get the schema once with:
    curl -sL https://json.schemastore.org/github-workflow.json -o gh-workflow.json
or, without internet access to that host:
    gh api repos/SchemaStore/schemastore/contents/src/schemas/json/github-workflow.json \\
        --jq .content | tr -d '\\n' | base64 -d > gh-workflow.json

Requires PyYAML:  pip install pyyaml   (jsonschema only for the schema pass)
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import yaml

IDENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_\-]*$")
EXPR = re.compile(r"\$\{\{(.*?)\}\}", re.S)

errors = []


def err(msg):
    errors.append(msg)
    print("ERROR  " + msg)


class GithubLoader(yaml.SafeLoader):
    """GitHub parses on/off/yes/no as strings; PyYAML's YAML 1.1 rules turn
    `on:` into the boolean True, which then fails schema validation."""


def fix_keys(doc):
    if isinstance(doc, dict) and True in doc:
        doc["on"] = doc.pop(True)
    return doc


def check_one_expression(path, where, expr):
    for prop in re.finditer(r"\.([A-Za-z0-9_\-]+)", expr):
        name = prop.group(1)
        if not IDENT.match(name):
            err(f"{path}: {where}: invalid property name '.{name}' in "
                f"'${{{{ {expr} }}}}' - names may not start with a digit "
                f"(use ['{name}'])")
    if re.search(r"\.\d+(?![\w])", expr):
        err(f"{path}: {where}: numeric property in '${{{{ {expr} }}}}'")


def check_expressions(path, text, doc):
    for m in EXPR.finditer(text):
        line = text[: m.start()].count("\n") + 1
        check_one_expression(path, f"line {line}", m.group(1).strip())

    # `if:` takes a bare expression, no ${{ }} needed
    def walk(node, trail):
        if isinstance(node, dict):
            for k, v in node.items():
                if k == "if" and isinstance(v, str):
                    check_one_expression(path, f"{trail}.if", v.strip())
                walk(v, f"{trail}.{k}")
        elif isinstance(node, list):
            for i, v in enumerate(node):
                walk(v, f"{trail}[{i}]")

    walk(doc, "")


def check_step_ids(path, doc):
    for job_name, job in (doc.get("jobs") or {}).items():
        seen = set()
        for i, step in enumerate(job.get("steps") or []):
            blob = json.dumps(step)
            for m in re.finditer(r"steps\.([A-Za-z0-9_\-]+)\.", blob):
                ref = m.group(1)
                if ref not in seen:
                    err(f"{path}: job '{job_name}' step {i} references "
                        f"steps.{ref} but no earlier step has that id")
            if "id" in step:
                seen.add(step["id"])


def check_shell(path, doc):
    for job_name, job in (doc.get("jobs") or {}).items():
        for i, step in enumerate(job.get("steps") or []):
            run = step.get("run")
            if not run:
                continue
            src = EXPR.sub(lambda m: "${" + m.group(1) + "}", run)
            with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as f:
                f.write(src)
                tmp = f.name
            try:
                r = subprocess.run(["bash", "-n", tmp], capture_output=True, text=True)
                if r.returncode != 0:
                    err(f"{path}: job '{job_name}' step {i} "
                        f"({step.get('name', 'unnamed')}): {r.stderr.strip()}")
            finally:
                os.unlink(tmp)


def check_schema(path, doc, schema):
    try:
        import jsonschema
    except ImportError:
        print(f"note: jsonschema not installed, skipping schema pass for {path}")
        return

    validator = jsonschema.Draft7Validator(schema)
    for e in sorted(validator.iter_errors(doc), key=lambda e: list(e.path)):
        where = "/".join(str(p) for p in e.path) or "(root)"
        err(f"{path}: schema: {where}: {e.message}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="*", help="workflow files (default: all)")
    ap.add_argument("--schema", default=os.environ.get("GITHUB_WORKFLOW_SCHEMA"),
                    help="path to github-workflow.json (schema pass is skipped "
                         "when unavailable)")
    args = ap.parse_args()

    paths = args.paths or sorted(glob.glob(".github/workflows/*.yml") +
                                 glob.glob(".github/workflows/*.yaml"))
    if not paths:
        print("no workflow files found")
        return 1

    schema = None
    if args.schema:
        with open(args.schema) as f:
            schema = json.load(f)

    for path in paths:
        print(f"--- {path}")
        with open(path) as f:
            text = f.read()
        try:
            doc = fix_keys(yaml.load(text, Loader=GithubLoader))
        except yaml.YAMLError as e:
            err(f"{path}: YAML: {e}")
            continue

        check_expressions(path, text, doc)
        check_step_ids(path, doc)
        check_shell(path, doc)
        if schema is not None:
            check_schema(path, doc, schema)

    if errors:
        print(f"\n{len(errors)} problem(s)")
        return 1
    print("\nno problems")
    return 0


if __name__ == "__main__":
    sys.exit(main())
