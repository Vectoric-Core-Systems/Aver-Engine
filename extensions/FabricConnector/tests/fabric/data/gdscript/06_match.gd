extends Node

enum Dir { UP, DOWN }
const LIMIT = 3

func literals(x):
	match x:
		1:
			print("one")
		2, 3:
			print("two or three")
		"hello":
			print("greeting")
		-1:
			print("minus one")
		1.5:
			print("float")
		true:
			print("bool")
		null:
			print("nothing")
		Dir.UP:
			print("up")
		LIMIT:
			print("limit")
		_:
			print("other")

func binding(x):
	match x:
		var v when v > 10:
			print("big ", v)
		var w:
			print("small ", w)

func guards(x, y):
	match x:
		1 when y > 0:
			print("a")
		2, 3 when y < 0:
			print("b")
		_ when y == 0:
			print("c")

func arrays(x):
	match x:
		[]:
			print("empty")
		[1]:
			print("one")
		[1, 2]:
			print("one two")
		[var a, var b]:
			print(a, b)
		[1, ..]:
			print("starts with one")
		[var first, _, ..]:
			print(first)
		[[1, 2], [var c, ..]]:
			print(c)

func dictionaries(x):
	match x:
		{}:
			print("empty")
		{"name": "Dennis"}:
			print("dennis")
		{"name": var n, "age": 18}:
			print(n)
		{"name": "Dennis", ..}:
			print("dennis and more")
		{"k": [var a, ..], "z": {"y": var q}}:
			print(a, q)

func inline_bodies(x):
	match x:
		1: print("one")
		2: return 2
		_: pass
