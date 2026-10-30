@tool
@icon("res://icon.svg")
## A script that declares everything a class body can hold.
class_name Declarations
extends "res://base/base_actor.gd"

signal died
signal hit(damage: int, source: Node3D)
signal moved(from, to := Vector3.ZERO)

enum State { IDLE, RUN = 5, JUMP, FALL = 1 << 3 }
enum { ANON_A, ANON_B = 2 }

const MAX_HEALTH = 100
const SPEED: float = 4.5
const GRAVITY := 9.8
const NAMES: Array[String] = ["a", "b"]
const LOOKUP = {"one": 1, "two": 2}

## The hit points.
@export var health: int = MAX_HEALTH
@export_range(0.0, 10.0, 0.1, "or_greater") var speed := SPEED
@export_enum("Warrior", "Mage", "Thief") var job: int
@export_group("Visual")
@export var tint: Color = Color.WHITE
@export var target: Node3D
@onready var sprite = $Sprite
@onready var timer: Timer = %Cooldown
var plain
var typed: Vector3
var inferred := Vector3(1, 2, 3)
var items: Array[int] = [1, 2, 3]
var table: Dictionary[String, int] = {}
var nested: Array[Array] = []
var other: Outer.Inner
static var counter := 0
static var shared: int = 7
@export var a: int = 1
@export var b: int = 2
@rpc("any_peer", "call_local", "reliable")
func remote_hit(amount): pass

class Weapon extends Node:
	var damage := 3
	func fire() -> void:
		pass
	class Muzzle:
		var flash = true

class Plain:
	signal ready_to_go
	const LIMIT = 2
