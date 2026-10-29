extends Node3D

var round_number := 0
var clock := 0.0

@rpc("authority", "call_local", "reliable")
func start_round(number: int) -> void:
	round_number = number

func _ready() -> void:
	if multiplayer.is_server():
		start_round.rpc(1)
