"""A tiny writer for Aver Node graphs (.ocgraph) that checks every LINK against the generated node reference
(docs/nodes/*.md), so a typo in a pin name fails the generator instead of failing at Compile time in the editor.

    g = Graph("AN_Driver", "Character", "A pawn that ...", class_attrs="view=firstperson")
    g.param("entity", "int"); g.param("deltaTime", "float")
    ent = g.node("ent", "Param", param="entity")
    ...
    g.link("tick", "exec", "gate", "exec")
    open(path, "w").write(g.text())

Used by make_vehicle_template.py (here) and test-content/VehicleLab/make_vehicle_lab.py. Not copied into projects.
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
NODE_DOCS = os.path.normpath(os.path.join(HERE, "..", "..", "docs", "nodes"))


def _split_cell(cell):
    """'in: `a` int, `b` float<br>out: `x` bool' -> ({'a': 'int', 'b': 'float'}, {'x': 'bool'})"""
    ins, outs = {}, {}
    if cell.strip() == "none":
        return ins, outs
    for part in cell.split("<br>"):
        part = part.strip()
        target = ins if part.startswith("in:") else outs if part.startswith("out:") else None
        if target is None:
            continue
        for m in re.finditer(r"`([A-Za-z0-9_]+)`\s+([A-Za-z0-9_]+)", part[part.index(":") + 1:]):
            target[m.group(1)] = m.group(2)
    return ins, outs


def load_node_pins():
    """type (lower case) -> (inputs, outputs, attribute names). Core.md rows have Pins in column 3, family rows in column 4."""
    table = {}
    if not os.path.isdir(NODE_DOCS):
        return table
    for fn in sorted(os.listdir(NODE_DOCS)):
        if not fn.endswith(".md") or fn.endswith(".notes.md") or fn == "README.md":
            continue
        with open(os.path.join(NODE_DOCS, fn), encoding="utf-8") as fh:
            for line in fh:
                if not line.startswith("| `"):
                    continue
                cells = [c.strip() for c in line.strip().strip("|").split(" | ")]
                if len(cells) < 5:
                    continue
                typ = cells[0].strip("`")
                family_row = len(cells) >= 7 and cells[3] in ("D", "P", "X")
                pins_cell = cells[4] if family_row else cells[3]
                attr_cell = cells[5] if family_row else cells[4]
                ins, outs = _split_cell(pins_cell)
                attrs = set(re.findall(r"`([A-Za-z0-9_|]+)`", attr_cell)) if attr_cell != "none" else set()
                table.setdefault(typ.lower(), (ins, outs, attrs))   # first wins: Core.md lists the gameplay vocabulary before the material graph's
    # Rows the generated pages do not list yet (a family added since `node-catalog.py --write` last ran) come straight from the C# tables.
    tables = os.path.normpath(os.path.join(HERE, "..", "..", "scripting", "csharp", "Aver.Graph"))
    row = re.compile(r'Reg\("(an_[a-z0-9_]+)",\s*GenericKind\.(Exec|Pure|Trigger),\s*typeof\(\w+\),\s*"\w+",\s*"([^"]*)",\s*"([^"]*)",\s*"([^"]*)"')
    if os.path.isdir(tables):
        for fn in sorted(os.listdir(tables)):
            if not (fn.startswith("Graph") and fn.endswith("NodeTable.cs")):
                continue
            with open(os.path.join(tables, fn), encoding="utf-8") as fh:
                for m in row.finditer(fh.read()):
                    key, kind, ins_s, outs_s, bind = m.groups()
                    if key in table:
                        continue
                    ins = {n.split(":")[0]: n.split(":")[1].split("=")[0] for n in ins_s.split(",") if ":" in n}
                    outs = {n.split(":")[0]: n.split(":")[1].split("=")[0] for n in outs_s.split(",") if ":" in n}
                    if kind == "Exec":
                        ins = dict([("exec", "exec")] + list(ins.items()))
                        outs = dict([("then", "exec")] + list(outs.items()))
                    attrs = set(t[1:] for t in bind.split(",") if t.startswith("@"))
                    table[key] = (ins, outs, attrs)
    return table


class Graph:
    def __init__(self, cls, parent, description, class_attrs="", header_comment=""):
        self.cls = cls
        self.parent = parent
        self.description = description
        self.class_attrs = class_attrs
        self.header_comment = header_comment
        self.params, self.vars, self.nodes, self.links, self.entries, self.outs = [], [], [], [], [], []
        self.types = {}
        self.pins = load_node_pins()
        self.problems = []

    def param(self, name, typ):
        self.params.append((name, typ))

    def var(self, name, typ, init):
        self.vars.append((name, typ, init))

    def node(self, ident, typ, **attrs):
        if ident in self.types:
            raise SystemExit("graph %s: node id '%s' used twice" % (self.cls, ident))
        self.types[ident] = typ
        known = self.pins.get(typ.lower())
        if known is None:
            self.problems.append("%s: node type '%s' is not in docs/nodes" % (ident, typ))
        else:
            for k in attrs:
                if k == "value" or k == "var" or k == "param":
                    continue
                if not any(k in a.split("|") for a in known[2]):
                    self.problems.append("%s (%s): attribute '%s' is not one of %s" % (ident, typ, k, sorted(known[2])))
        self.nodes.append((ident, typ, attrs))
        return ident

    def entry(self, ident, name):
        self.entries.append((ident, name))

    def out(self, ident, pin):
        self.outs.append((ident, pin))

    def link(self, src, src_pin, dst, dst_pin):
        for ident, pin, want_out in ((src, src_pin, True), (dst, dst_pin, False)):
            typ = self.types.get(ident)
            if typ is None:
                self.problems.append("link %s.%s -> %s.%s: unknown node '%s'" % (src, src_pin, dst, dst_pin, ident))
                return
            known = self.pins.get(typ.lower())
            if known is None:
                continue
            ins, outs, _ = known
            pool = outs if want_out else ins
            if typ.lower() in ("param", "getvar") and want_out and pin == "value":
                continue
            if typ.lower() in ("onstart", "ontick", "oninputaction", "onhit", "customevent", "an_onevent") and want_out and pin == "exec":
                continue
            if pin not in pool:
                self.problems.append("link %s.%s -> %s.%s: node %s (%s) has no %s pin '%s' (has %s)" %
                                     (src, src_pin, dst, dst_pin, ident, typ, "output" if want_out else "input", pin, sorted(pool)))
        self.links.append((src, src_pin, dst, dst_pin))

    def text(self):
        if self.problems:
            raise SystemExit("graph %s has problems:\n  " % self.cls + "\n  ".join(self.problems))
        o = ["OCGRAPH 1", "DOMAIN gameplay"]
        if self.header_comment:
            o += ["# " + l if l else "#" for l in self.header_comment.strip("\n").split("\n")]
        o.append("NAME " + self.cls.replace("AN_", "", 1))
        o.append("DESCRIPTION " + self.description)
        o.append("")
        o.append("CLASS %s %s%s" % (self.cls, self.parent, (" " + self.class_attrs) if self.class_attrs else ""))
        o.append("")
        for n, t in self.params:
            o.append("PARAM %s %s" % (n, t))
        for n, t, i in self.vars:
            o.append("VAR %s %s %s" % (n, t, i))
        if self.params or self.vars:
            o.append("")
        for ident, name in self.entries:
            o.append("ENTRY %s %s" % (ident, name))
        for ident, typ, attrs in self.nodes:
            o.append("NODE %s %s%s" % (ident, typ, "".join(" %s=%s" % (k, v) for k, v in attrs.items())))
        for a, ap, b, bp in self.links:
            o.append("LINK %s.%s %s.%s" % (a, ap, b, bp))
        for ident, pin in self.outs:
            o.append("OUT %s %s" % (ident, pin))
        return "\n".join(o) + "\n"
