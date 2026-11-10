"""graphgen: write .ocgraph gameplay graphs from Python, checked against the node reference.

The .ocgraph format is one record per node, pin value and wire, which is right for a canvas and slow and error-prone
by hand: the parser skips unknown records and the compiler reads a misspelt pin as unconnected, so a typo is silent.
This builds the same records from compact Python and refuses, at generation time, any node type, pin, attribute or
wire the generated node reference (docs/nodes/*.md) does not list.

    from graphgen import Graph
    g = Graph("Clock", cls="AN_Clock GameMode")
    g.param("deltaTime", "float")
    g.var("elapsed", "float", 0)
    tick = g.event("OnTick")
    total = g.node("Add", a=g.get("elapsed"), b=g.node("Param", param="deltaTime").value)
    g.chain(tick, g.set("elapsed", total.result))
    g.write("Content/Scripts/AN_Clock.ocgraph")

Inputs given as a Pin are wired, as a Python number or bool are written as PINVAL (floats always with a decimal
point: the parser reads `5` as an int). Keyword arguments that name an attribute of the node type become attributes.
Exec flow: g.chain(a, b, c) wires a's exec output to b's exec input, and so on; a Pin names a specific exec output
(branch.true). Node pages: docs/nodes/README.md.
"""
from __future__ import annotations

import os
import re

_HERE = os.path.dirname(os.path.abspath(__file__))
_DOCS = os.path.normpath(os.path.join(_HERE, "..", "..", "docs", "nodes"))

# Pins typed by the VAR they name, not by the catalog's placeholder (GetVar/SetVar list `value` float).
_VAR_TYPED = {"GetVar": "value", "SetVar": "value"}
_NUMERIC = {"float", "int"}


def _cells(line: str) -> list[str]:
    """Split a table row on `|`, except inside backticks (an attribute cell reads `path|name|text`)."""
    cells, cur, tick = [], [], False
    for ch in line.strip().strip("|"):
        if ch == "`":
            tick = not tick
        if ch == "|" and not tick:
            cells.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    cells.append("".join(cur).strip())
    return cells


def _pins(cell: str) -> tuple[list[tuple[str, str]], list[tuple[str, str]]]:
    ins, outs = [], []
    for part in cell.split("<br>"):
        part = part.strip()
        dest = ins if part.startswith("in:") else outs if part.startswith("out:") else None
        if dest is None:
            continue
        for m in re.finditer(r"`([^`]+)`\s+([A-Za-z0-9]+)", part):
            dest.append((m.group(1), m.group(2)))
    return ins, outs


def _attrs(cell: str) -> set[str]:
    out = set()
    for tok in re.findall(r"`([^`]+)`", cell):
        for alt in tok.split("|"):
            out.add(alt.strip().rstrip("?"))
    return out


def load_catalog(docs: str = _DOCS) -> dict:
    """{type: {"in": {pin: type}, "out": {pin: type}, "inOrder": [...], "attrs": set}} for gameplay nodes."""
    cat: dict = {}
    for fn in sorted(os.listdir(docs)):
        if not fn.endswith(".md") or fn == "README.md" or fn.endswith(".notes.md"):
            continue
        cols = None
        in_material = False
        for line in open(os.path.join(docs, fn), encoding="utf-8"):
            if line.startswith("## "):
                in_material = "aterial" in line
            if in_material or not line.startswith("|"):
                continue
            c = _cells(line)
            if c and c[0] == "Type":
                cols = {name: i for i, name in enumerate(c)}
                continue
            if cols is None or not c[0].startswith("`"):
                continue
            t = c[0].strip("`")
            ins, outs = _pins(c[cols["Pins"]])
            attrs = _attrs(c[cols["Attributes"]]) if "Attributes" in cols else set()
            cat.setdefault(t, {"in": dict(ins), "out": dict(outs), "inOrder": [p for p, _ in ins], "attrs": attrs})
    return cat


CATALOG = load_catalog()


class GraphError(Exception):
    pass


class Pin:
    __slots__ = ("node", "name", "type")

    def __init__(self, node: "Node", name: str, type_: str):
        self.node, self.name, self.type = node, name, type_

    def __repr__(self):
        return f"{self.node.id}.{self.name}"


class Node:
    def __init__(self, graph: "Graph", id_: str, type_: str, spec: dict):
        self.graph, self.id, self.type, self.spec = graph, id_, type_, spec
        self.attrs: dict[str, str] = {}

    def __getattr__(self, name: str) -> Pin:
        if name.startswith("_") or name in ("graph", "id", "type", "spec", "attrs"):
            raise AttributeError(name)
        return self.out(name)

    def out(self, name: str) -> Pin:
        t = self.spec["out"].get(name)
        if t is None:
            raise GraphError(f"{self.type} '{self.id}' has no output '{name}' (outputs: {', '.join(self.spec['out']) or 'none'})")
        if self.type in _VAR_TYPED and name == _VAR_TYPED[self.type]:
            t = self.graph.vars[self.attrs["var"]][0]
        return Pin(self, name, t)

    def inp(self, name: str) -> Pin:
        t = self.spec["in"].get(name)
        if t is None:
            raise GraphError(f"{self.type} '{self.id}' has no input '{name}' (inputs: {', '.join(self.spec['in']) or 'none'})")
        if self.type in _VAR_TYPED and name == _VAR_TYPED[self.type]:
            t = self.graph.vars[self.attrs["var"]][0]
        return Pin(self, name, t)

    def exec_out(self) -> Pin:
        for name in ("then", "exec"):
            if self.spec["out"].get(name) == "exec":
                return Pin(self, name, "exec")
        raise GraphError(f"{self.type} '{self.id}' has no exec output to chain from")


class Graph:
    def __init__(self, name: str, cls: str | None = None, desc: str = "", domain: str = "gameplay", header: str = ""):
        if " " in name:
            raise GraphError("NAME takes one token")
        self.name, self.cls, self.desc, self.domain, self.header = name, cls, desc, domain, header
        self.params: list[tuple[str, str]] = []
        self.vars: dict[str, tuple[str, object]] = {}
        self.funcs: list[tuple[str, bool, list, list]] = []
        self.comps: list[str] = []
        self.nodes: list[Node] = []
        self.links: list[tuple[Pin, Pin]] = []
        self.pinvals: list[tuple[Node, str, object]] = []
        self.entries: list[tuple[str, str]] = []
        self.outs: list[Pin] = []
        self._ids: dict[str, int] = {}
        self._used_ids: set[str] = set()
        self._self: Node | None = None
        self._params: dict[str, Node] = {}
        self._consts: dict[tuple[str, str], Node] = {}

    # ---- declarations ------------------------------------------------------------------------------------
    def param(self, name: str, type_: str):
        self.params.append((name, type_))

    def var(self, name: str, type_: str, default=0):
        if type_ not in ("float", "int", "bool"):
            raise GraphError(f"VAR {name}: type {type_} (float, int or bool)")
        if name in self.vars:
            raise GraphError(f"VAR {name} declared twice")
        self.vars[name] = (type_, default)

    def comp(self, line: str):
        self.comps.append(line)

    # ---- nodes -------------------------------------------------------------------------------------------
    def _new_id(self, type_: str, id_: str | None) -> str:
        if id_ is None:
            base = re.sub(r"^AN_", "", type_)
            base = base[:1].lower() + base[1:]
            n = self._ids.get(base, 0) + 1
            self._ids[base] = n
            id_ = f"{base}{n}"
        if id_ in self._used_ids:
            raise GraphError(f"node id '{id_}' used twice")
        self._used_ids.add(id_)
        return id_

    def node(self, type_: str, id: str | None = None, **kw) -> Node:
        spec = CATALOG.get(type_)
        if spec is None:
            close = [t for t in CATALOG if t.lower() == type_.lower() or t.lower() == "an_" + type_.lower()]
            raise GraphError(f"unknown node type '{type_}'" + (f" (did you mean {close[0]}?)" if close else ""))
        n = Node(self, self._new_id(type_, id), type_, spec)
        # Attributes first: a VAR-typed pin needs n.attrs["var"] to know its type.
        for k, v in list(kw.items()):
            if k not in spec["in"] and k in spec["attrs"]:
                n.attrs[k] = str(v)
                del kw[k]
        if type_ in _VAR_TYPED:
            var = n.attrs.get("var")
            if var not in self.vars:
                raise GraphError(f"{type_} '{n.id}': VAR '{var}' is not declared")
        self.nodes.append(n)
        for k, v in kw.items():
            if k not in spec["in"]:
                raise GraphError(f"{type_} '{n.id}': no input or attribute '{k}' "
                                 f"(inputs: {', '.join(spec['in']) or 'none'}; attributes: {', '.join(sorted(spec['attrs'])) or 'none'})")
            self._feed(n.inp(k), v)
        return n

    def _feed(self, dst: Pin, v):
        if isinstance(v, Node):
            outs = [p for p, t in v.spec["out"].items() if t != "exec"]
            if len(outs) != 1:
                raise GraphError(f"{v.type} '{v.id}' has {len(outs)} data outputs; name one for {dst}")
            v = v.out(outs[0])
        if isinstance(v, Pin):
            self.link(v, dst)
            return
        if isinstance(v, bool):
            if dst.type != "bool":
                raise GraphError(f"{dst} is {dst.type}, given a bool")
        elif isinstance(v, (int, float)):
            if dst.type not in _NUMERIC:
                raise GraphError(f"{dst} is {dst.type}, given a number")
            if dst.type == "int" and (isinstance(v, float) and not v.is_integer()):
                raise GraphError(f"{dst} is int, given {v}")
        else:
            raise GraphError(f"{dst}: cannot feed {v!r}")
        # A shared Const node per value, the form every template graph uses, rather than a PINVAL record.
        if dst.type == "bool":
            self.link(self.b(bool(v)), dst)
        elif dst.type == "int":
            self.link(self.i(int(v)), dst)
        else:
            self.link(self.f(float(v)), dst)

    _CONVERT = {("int", "float"): "IntToFloat", ("float", "int"): "FloatToInt", ("bool", "float"): "BoolToFloat"}

    def link(self, src: Pin, dst: Pin):
        if (src.type == "exec") != (dst.type == "exec"):
            raise GraphError(f"LINK {src} -> {dst}: exec and data mixed")
        conv = self._CONVERT.get((src.type, dst.type))
        if conv:   # an explicit conversion node rather than trusting the compiler to coerce a wire
            src = self.node(conv, a=src).result
        if src.type != dst.type:
            raise GraphError(f"LINK {src} ({src.type}) -> {dst} ({dst.type}): type mismatch")
        if src.type != "exec" and any(d.node is dst.node and d.name == dst.name for _, d in self.links):
            raise GraphError(f"{dst} is wired twice")
        self.links.append((src, dst))

    # ---- sugar ------------------------------------------------------------------------------------------
    def self_(self) -> Pin:
        if self._self is None:
            self._self = self.node("Param", id="self", param="entity")
            if ("entity", "int") not in self.params:
                self.params.insert(0, ("entity", "int"))
        return Pin(self._self, "value", "int")

    def p(self, name: str) -> Pin:
        """The value of a PARAM (entity, time, deltaTime)."""
        if name == "entity":
            return self.self_()
        if name not in self._params:
            self._params[name] = self.node("Param", id=f"p_{name}", param=name)
            if not any(n == name for n, _ in self.params):
                self.params.append((name, "float"))
        return Pin(self._params[name], "value", dict(self.params)[name])

    def get(self, var: str) -> Pin:
        return self.node("GetVar", var=var).value

    def set(self, var: str, value) -> Node:
        return self.node("SetVar", var=var, value=value)

    def _const(self, type_: str, text: str, ptype: str) -> Pin:
        key = (type_, text)
        n = self._consts.get(key)
        if n is None:
            safe = re.sub(r"[^A-Za-z0-9]", "_", text)
            n = self._consts[key] = self.node(type_, id=f"k{type_[5].lower()}_{safe}", value=text)
        return Pin(n, "value", ptype)

    def f(self, v: float) -> Pin:
        return self._const("ConstFloat", repr(float(v)), "float")

    def i(self, v: int) -> Pin:
        return self._const("ConstInt", str(int(v)), "int")

    def b(self, v: bool) -> Pin:
        return self._const("ConstBool", "true" if v else "false", "bool")

    def event(self, name: str, id: str | None = None, bus: bool = False) -> Node:
        """An ENTRY: OnStart/OnTick by their own node type; `bus` for an AN_OnEvent that hears the bus event of that
        name (AN_DispatchEvent, with a payload); anything else as a CustomEvent of that name (FireEvent, AN_SetTimer)."""
        type_ = name if name in ("OnStart", "OnTick") else ("AN_OnEvent" if bus else "CustomEvent")
        n = self.node(type_, id=id or f"ev_{re.sub(r'[^A-Za-z0-9]', '_', name)}")
        self.entries.append((n.id, name))
        return n

    def chain(self, *steps):
        """Wire exec: each step's exec output to the next step's exec input. A Pin step is a specific exec output."""
        prev = None
        for s in steps:
            if s is None:
                continue
            if prev is not None:
                src = prev if isinstance(prev, Pin) else prev.exec_out()
                if not isinstance(s, Node):
                    raise GraphError(f"chain: {s!r} is not a node")
                self.link(src, s.inp("exec"))
            prev = s
        return prev

    def output(self, pin: Pin):
        self.outs.append(pin)

    # ---- output -----------------------------------------------------------------------------------------
    def text(self) -> str:
        L = ["OCGRAPH 1", f"DOMAIN {self.domain}"]
        for h in self.header.strip("\n").splitlines():
            L.append("# " + h if h else "#")
        L.append(f"NAME {self.name}")
        if self.desc:
            L.append(f"DESCRIPTION {self.desc}")
        if self.cls:
            L.append(f"CLASS {self.cls}")
        L.append("")
        for n, t in self.params:
            L.append(f"PARAM {n} {t}")
        for n, (t, d) in self.vars.items():
            dv = ("true" if d else "false") if t == "bool" else (str(int(d)) if t == "int" else repr(float(d)))
            L.append(f"VAR {n} {t} {dv}")
        L.extend(self.comps)
        L.append("")
        for n in self.nodes:
            L.append(" ".join([f"NODE {n.id} {n.type}"] + [f"{k}={v}" for k, v in n.attrs.items()]))
        for node, pin, v in self.pinvals:
            s = ("true" if v else "false") if isinstance(v, bool) else (str(v) if isinstance(v, int) and node.spec["in"][pin] == "int" else repr(float(v)))
            L.append(f"PINVAL {node.id} {pin} {s}")
        for src, dst in self.links:
            L.append(f"LINK {src} {dst}")
        for nid, ev in self.entries:
            L.append(f"ENTRY {nid} {ev}")
        for p in self.outs:
            L.append(f"OUT {p.node.id} {p.name}")
        return "\n".join(L) + "\n"

    def check(self):
        ids = {n.id for n in self.nodes}
        for _, ev in self.entries:
            pass
        for src, dst in self.links:
            assert src.node.id in ids and dst.node.id in ids
        # Every exec-input node (a P node) that is not reachable from an entry never runs: flag it.
        exec_nodes = [n for n in self.nodes if n.spec["in"].get("exec") == "exec"]
        fed = {dst.node.id for src, dst in self.links if dst.name == "exec"}
        dead = [n.id for n in exec_nodes if n.id not in fed]
        if dead:
            raise GraphError(f"{self.name}: exec nodes never reached: {', '.join(dead[:12])}")

    def write(self, path: str):
        self.check()
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        with open(path, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(self.text())
        return path


class Flow:
    """An exec cursor: the exec output the next node will be wired from.

    BRANCHES NEVER MERGE. The push compiler inlines a node once per path that reaches it (GraphCompiler.ExecDispatch's
    EmitExecNode: a diamond is emitted per arm, there are no jumps), so N branches that rejoin the main chain cost 2^N
    copies of everything after them -- a 40-branch tick graph hung the editor and took 29 GB at Play. So each if_/switch
    is a statement: a Sequence runs the Branch from then0 and the rest of the chain continues from then1, and the
    arms end where they end. Order is unchanged (then0 runs to completion before then1) and the size stays linear.

        fl = Flow(g, tick)
        fl.then(g.set("a", 1.0))
        fl.if_(cond, lambda t: t.then(...), lambda f: f.then(...))
        fl.then(...)                       # runs after either branch
    """

    def __init__(self, g: Graph, *starts):
        self.g = g
        self.exits: list[Pin] = []
        for s in starts:
            self.exits.append(s if isinstance(s, Pin) else s.exec_out())

    def _wire(self, node: Node):
        for e in self.exits:
            self.g.link(e, node.inp("exec"))

    def then(self, node: Node) -> Node:
        self._wire(node)
        self.exits = [node.exec_out()]
        return node

    def do(self, *nodes) -> "Flow":
        for n in nodes:
            if n is not None:
                self.then(n)
        return self

    def _statement(self):
        seq = self.g.node("Sequence")
        self._wire(seq)
        self.exits = [seq.out("then1")]
        return seq.out("then0")

    def if_(self, cond, then=None, else_=None) -> "Flow":
        br = self.g.node("Branch", cond=cond)
        self.g.link(self._statement(), br.inp("exec"))
        if then:
            then(Flow(self.g, br.true))
        if else_:
            else_(Flow(self.g, br.false))
        return self

    def switch(self, selector, cases: dict, default=None) -> "Flow":
        """SwitchInt over cases 0..3 (the node's width); anything else goes to `default`."""
        sw = self.g.node("SwitchInt", selector=selector)
        self.g.link(self._statement(), sw.inp("exec"))
        for k in range(4):
            if k in cases and cases[k]:
                cases[k](Flow(self.g, sw.out(f"case{k}")))
        if default:
            default(Flow(self.g, sw.out("default")))
        return self

    def dead(self) -> bool:
        return not self.exits
