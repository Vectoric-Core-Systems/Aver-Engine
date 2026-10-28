class_name Enemy
extends CharacterBody3D
## A third realistic script for the F4b tests: states, patrol points, a spawned bullet, groups, a dictionary of stats.

signal died(enemy: Enemy)
signal damaged(amount: float, remaining: float)

enum State { PATROL, CHASE, ATTACK, DEAD }

const SPEED := 3.0
const ATTACK_RANGE := 2.0
const GROUP := "enemies"

@export var max_health := 50.0
@export var patrol_points: Array[Vector3] = []
@export var bullet_scene: PackedScene
@export_node_path("Node3D") var target_path: NodePath

var health := max_health
var state: State = State.PATROL
var target: Node3D
var patrol_index := 0
var _cooldown := 0.0
var stats := {"kills": 0, "shots": 0}

@onready var nav: Node3D = $Nav
@onready var timer: Timer = $Timer

func _ready() -> void:
	add_to_group(GROUP)
	health = max_health
	if not target_path.is_empty():
		target = get_node_or_null(target_path) as Node3D
	timer.timeout.connect(_on_think)
	timer.start(0.5)

func _physics_process(delta: float) -> void:
	_cooldown = maxf(_cooldown - delta, 0.0)
	match state:
		State.PATROL:
			_patrol(delta)
		State.CHASE:
			_chase(delta)
		State.ATTACK:
			velocity = Vector3.ZERO
			if _cooldown <= 0.0:
				_shoot()
		State.DEAD:
			return
	if not is_on_floor():
		velocity.y -= 9.8 * delta
	move_and_slide()

func _patrol(delta: float) -> void:
	if patrol_points.is_empty():
		return
	var goal := patrol_points[patrol_index]
	var to_goal := goal - global_position
	to_goal.y = 0.0
	if to_goal.length() < 0.5:
		patrol_index = (patrol_index + 1) % patrol_points.size()
	else:
		velocity = to_goal.normalized() * SPEED

func _chase(delta: float) -> void:
	if target == null or not is_instance_valid(target):
		state = State.PATROL
		return
	var dir := target.global_position - global_position
	if dir.length() <= ATTACK_RANGE:
		state = State.ATTACK
	else:
		velocity = dir.normalized() * SPEED * 1.5

func _shoot() -> void:
	_cooldown = 1.0
	stats["shots"] += 1
	if bullet_scene:
		var bullet := bullet_scene.instantiate() as Node3D
		get_parent().add_child(bullet)
		bullet.global_position = global_position + Vector3.UP
		bullet.look_at(target.global_position)

func take_damage(amount: float) -> void:
	if state == State.DEAD:
		return
	health -= amount
	damaged.emit(amount, health)
	if health <= 0.0:
		die()

func die() -> void:
	state = State.DEAD
	stats["kills"] = int(stats.get("kills", 0)) + 1
	died.emit(self)
	await get_tree().create_timer(2.0).timeout
	queue_free()

func _on_think() -> void:
	if target and state != State.DEAD:
		var d := global_position.distance_to(target.global_position)
		if d < 15.0:
			state = State.CHASE
		elif state == State.CHASE:
			state = State.PATROL
