extends CharacterBody3D

signal hit(amount)

@export var health := 100
@export var player_name := ""

var shots := 0

@rpc("any_peer", "call_local", "reliable")
func set_player_name(new_name: String) -> void:
	player_name = new_name

@rpc
func authority_ping(n: int) -> void:
	shots += n

@rpc("any_peer")
func unreliable_default(x: float) -> void:
	shots += int(x)

@rpc("authority", "call_remote", "unreliable_ordered", 2)
func state(pos: Vector3) -> void:
	position = pos

@rpc("any_peer", "call_local", "unreliable")
func shoot(dir: Vector3) -> void:
	shots += 1

@rpc("reliable", "call_local")
func reorder() -> void:
	shots += 10

@rpc("disabled")
func never() -> void:
	pass

func _enter_tree() -> void:
	set_multiplayer_authority(str(name).to_int())

func _ready() -> void:
	if is_multiplayer_authority():
		set_player_name.rpc("host")
	shoot.rpc_id(1, Vector3.ZERO)
	rpc("authority_ping", 1)
	rpc_id(1, "unreliable_default", 2.0)
	var id := multiplayer.get_unique_id()
	if multiplayer.is_server():
		hit.emit(id)
