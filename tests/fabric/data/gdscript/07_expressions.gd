extends Node

const Bullet = preload("res://bullet.tscn")
const Config = preload("res://config.gd")

@onready var label = $UI/Label
@onready var camera: Camera3D = $"../Camera Rig/Camera3D"
@onready var bar = %HealthBar
@onready var deep = $Pivot/%Marker

var table := {"a": 1, "b": [1, 2], "c": {"d": 2}}
var lua := {name = "bob", age = 3}
var arr := [1, 2.5, "three", [4], {"five": 5}, null, true]
var typed_arr: Array[Node3D] = []
var typed_dict: Dictionary[String, Array] = {}
var made = Array[int]()

func arithmetic(a, b, c):
	var r1 = a + b * c
	var r2 = (a + b) * c
	var r3 = a - b - c
	var r4 = a ** b ** c
	var r5 = -a ** 2
	var r6 = 2 ** -1
	var r7 = a % b
	var r8 = a / b * c
	var r9 = a << 2 | b & c ^ 1
	var r10 = ~a + 1
	var r11 = -(-a)
	var r12 = a == b and b != c or not a
	var r13 = a && b || !c
	var r14 = a < b and b <= c and c > a and c >= a
	var r15 = a in b
	var r16 = a not in b
	var r17 = not a in b
	var r18 = a is Node3D
	var r19 = a as Node3D
	var r20 = (a as Node3D).position
	var r21 = a if b else c
	var r22 = "x %s y %d" % [a, b]
	var r23 = "single %s" % a
	var r24 = a is Array[int]
	return r1

func assignments(a):
	a = 1
	a += 1
	a -= 1
	a *= 2
	a /= 2
	a %= 3
	a **= 2
	a &= 1
	a |= 2
	a ^= 3
	a <<= 1
	a >>= 1
	a.b.c = 4
	a[0] = 5
	a[0][1] += 6
	self.value = 7
	$Node.visible = false

func calls(a):
	print()
	print(1, 2,)
	a.method(1)(2)
	a.b.c().d[0].e()
	get_node("Path").queue_free()
	$Mesh.get_surface_override_material(0).albedo_color = Color.RED
	load("res://x.tscn").instantiate()
	Bullet.instantiate()
	super.foo(1)
	Vector3(1, 2, 3).normalized() * 5.0
	return await get_tree().process_frame

func atoms(a):
	var s = self
	var n = null
	var t = [true, false]
	var p = PI + TAU + INF
	var path = ^"Foo/Bar"
	var sn = &"action"
	var lines = a.size() \
		+ 2
	var multi = (
		a
		+ 1
	)
	return [s, n, t, p, path, sn, lines, multi]
