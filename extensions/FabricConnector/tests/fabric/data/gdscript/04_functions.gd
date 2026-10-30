extends Node3D
class_name Functions

func no_args():
	pass

func typed(a: int, b: float = 2.0, c := "x", d = null) -> bool:
	return a > 0

func returns_void() -> void:
	return

func returns_array() -> Array[int]:
	return []

func returns_dict() -> Dictionary[String, Node3D]:
	return {}

func returns_inner() -> Outer.Inner:
	return null

static func make(kind: StringName) -> Functions:
	return Functions.new()

func variadic(first, ...rest: Array):
	return rest.size()

func trailing(a,
		b,
		c = 3,
	):
	pass

func _ready() -> void:
	super()
	super._ready()
	var x := await get_tree().create_timer(0.5).timeout
	await $Anim.animation_finished
	var y = await some_signal
	print(x, y)

func _init(value = 1):
	super(value)

func one_liner(): return 42

func multi_statement(): var a = 1; var b = 2; return a + b

func with_locals() -> int:
	const LIMIT = 10
	var total := 0
	var typed_local: int = 5
	total += typed_local
	assert(total > 0)
	assert(total < LIMIT, "too big")
	@warning_ignore("unused_variable")
	var ignored = 1
	return total
