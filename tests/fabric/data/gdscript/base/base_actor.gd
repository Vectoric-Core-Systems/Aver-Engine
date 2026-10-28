class_name BaseActor
extends Node3D
## The base class 02_declarations.gd extends by path.

var base_health := 10

func _ready() -> void:
	base_health += 1

func describe() -> String:
	return "actor with %d health" % base_health
