class_name Counter
extends Node
## A node the F4b tests add to the tree: @export, @onready, setget, signals, await and the tick virtuals.

signal changed(old_value, new_value)
signal finished

@export var step := 2
@export_range(0, 10) var limit: int = 10
var total := 0:
	set(value):
		var old := total
		total = value
		changed.emit(old, total)
var entries: Array[String] = []
var ticks := 0
@onready var helper: Timer = $Helper

func _ready() -> void:
	entries.append("ready")
	helper.wait_time = 0.25
	helper.one_shot = true

func add(n: int) -> void:
	total += n * step
	entries.append("add %d" % n)

func countdown(from: int) -> int:
	var sum := 0
	for i in range(from, 0, -1):
		sum += i
	return sum

func wait_and_finish() -> void:
	helper.start()
	await helper.timeout
	entries.append("waited")
	finished.emit()

func _process(_delta: float) -> void:
	ticks += 1
