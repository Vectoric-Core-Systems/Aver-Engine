extends Node

var inline_lambda = func(x): return x * 2
var typed_lambda = func(a: int, b := 2) -> int: return a + b
var named_lambda = func fact(n): return 1 if n <= 1 else n * fact(n - 1)
var block_lambda = func(x):
	var y = x + 1
	return y

var after_lambda = 5

func connect_things(button: Button, items: Array):
	button.pressed.connect(func(): print("pressed"))
	button.pressed.connect(func():
		print("multi")
		queue_free()
	)
	button.toggled.connect(func(on: bool):
		if on:
			print("on")
		else:
			print("off")
	)
	items.map(func(i): return i * 2)
	items.filter(func(i):
		return i > 1
	).map(func(i): return str(i))
	items.sort_custom(func(a, b):
		return a < b)
	var total = items.reduce(func(acc, i): return acc + i, 0)
	var pair = [func(): return 1, func(): return 2]
	var d = {"f": func(x): return x, "g": 3}
	call_deferred("foo", func():
		for i in 3:
			print(i)
	, 5)
	var outer = func(x):
		var inner = func(y):
			return x + y
		return inner.call(1)
	var t = get_tree().create_timer(1.0)
	t.timeout.connect(func():
		var nested = func():
			pass
		nested.call()
	)
	return outer
