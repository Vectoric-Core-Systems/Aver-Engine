extends Node

func branches(x: int) -> String:
	if x < 0:
		return "neg"
	elif x == 0:
		return "zero"
	elif x < 10: return "small"
	else:
		return "big"

func loops(items: Array):
	var n := 0
	while n < 10:
		n += 1
		if n == 3:
			continue
		if n > 7:
			break
	for i in range(5):
		print(i)
	for i: int in [1, 2, 3]:
		print(i)
	for k in {"a": 1}:
		pass
	for child in get_children(): child.queue_free()
	while true: break

func nested():
	for i in 3:
		for j in 3:
			if i == j:
				pass
			else:
				print(i, j)

func ternaries(a, b):
	var m = a if a > b else b
	var s = "x" if a else "y" if b else "z"
	return -1 if a < 0 else 1

func inline_if(x):
	if x: print("yes")
	if x: print("a"); print("b")
	else: print("no")

func misc():
	pass; pass
	breakpoint
	return
