class_name PlayerController
extends CharacterBody3D
## First-person controller with a dash and a coyote-time jump.

signal jumped
signal landed(impact: float)
signal health_changed(old_value: int, new_value: int)
signal died

enum State { IDLE, WALK, SPRINT, AIR, DEAD }

const WALK_SPEED := 5.0
const SPRINT_SPEED: float = 8.0
const JUMP_VELOCITY = 4.5
const COYOTE_TIME = 0.12
const MOUSE_SENSITIVITY := 0.002

@export_group("Movement")
@export var acceleration: float = 12.0
@export_range(0.0, 1.0, 0.01) var air_control := 0.3
@export var can_dash := true
@export_group("Combat")
@export var max_health: int = 100
@export var hit_sound: AudioStream
@export_flags("Fire", "Ice", "Poison") var resistances: int = 0

@onready var head: Node3D = $Head
@onready var camera: Camera3D = $Head/Camera3D
@onready var ray: RayCast3D = $Head/Camera3D/RayCast3D
@onready var anim: AnimationPlayer = %AnimationPlayer
@onready var dash_timer: Timer = $DashTimer

var state: State = State.IDLE
var gravity: float = ProjectSettings.get_setting("physics/3d/default_gravity")
var coyote := 0.0
var health: int = 100:
	set(value):
		var old := health
		health = clampi(value, 0, max_health)
		health_changed.emit(old, health)
		if health == 0 and state != State.DEAD:
			_die()
var _was_on_floor := false
var inventory: Dictionary[String, int] = {}
var tags: Array[StringName] = [&"player", &"alive"]

func _ready() -> void:
	Input.mouse_mode = Input.MOUSE_MODE_CAPTURED
	dash_timer.timeout.connect(_on_dash_timer_timeout)
	add_to_group("player")

func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventMouseMotion and Input.mouse_mode == Input.MOUSE_MODE_CAPTURED:
		rotate_y(-event.relative.x * MOUSE_SENSITIVITY)
		head.rotate_x(-event.relative.y * MOUSE_SENSITIVITY)
		head.rotation.x = clamp(head.rotation.x, -PI / 2.0, PI / 2.0)
	elif event.is_action_pressed("ui_cancel"):
		Input.mouse_mode = Input.MOUSE_MODE_VISIBLE

func _physics_process(delta: float) -> void:
	if state == State.DEAD:
		return
	if not is_on_floor():
		velocity.y -= gravity * delta
		coyote -= delta
	else:
		coyote = COYOTE_TIME

	if Input.is_action_just_pressed("jump") and coyote > 0.0:
		velocity.y = JUMP_VELOCITY
		coyote = 0.0
		jumped.emit()

	var input_dir := Input.get_vector("move_left", "move_right", "move_forward", "move_back")
	var direction := (transform.basis * Vector3(input_dir.x, 0, input_dir.y)).normalized()
	var speed := SPRINT_SPEED if Input.is_action_pressed("sprint") else WALK_SPEED
	var control := 1.0 if is_on_floor() else air_control
	if direction:
		velocity.x = move_toward(velocity.x, direction.x * speed, acceleration * control * delta)
		velocity.z = move_toward(velocity.z, direction.z * speed, acceleration * control * delta)
	else:
		velocity.x = move_toward(velocity.x, 0.0, acceleration * delta)
		velocity.z = move_toward(velocity.z, 0.0, acceleration * delta)

	move_and_slide()
	_update_state()
	if is_on_floor() and not _was_on_floor:
		landed.emit(absf(velocity.y))
	_was_on_floor = is_on_floor()

func _update_state() -> void:
	match state:
		State.DEAD:
			return
		_:
			if not is_on_floor():
				state = State.AIR
			elif velocity.length() < 0.1:
				state = State.IDLE
			elif Input.is_action_pressed("sprint"):
				state = State.SPRINT
			else:
				state = State.WALK

func dash(direction: Vector3) -> void:
	if not can_dash:
		return
	can_dash = false
	velocity += direction * 20.0
	dash_timer.start()
	await dash_timer.timeout
	can_dash = true

func take_damage(amount: int, source: Node = null) -> void:
	health -= amount
	if source and source.has_method("on_hit_confirmed"):
		source.on_hit_confirmed(self)
	if hit_sound:
		var player := AudioStreamPlayer3D.new()
		player.stream = hit_sound
		add_child(player)
		player.play()
		player.finished.connect(player.queue_free)

func pick_up(item: StringName, count := 1) -> bool:
	inventory[item] = inventory.get(item, 0) + count
	return true

func _die() -> void:
	state = State.DEAD
	died.emit()
	anim.play("death")
	await anim.animation_finished
	get_tree().reload_current_scene()

func _on_dash_timer_timeout() -> void:
	can_dash = true
