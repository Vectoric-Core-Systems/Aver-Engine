extends Node

var peer := ENetMultiplayerPeer.new()
var players: Array[int] = []

func host(port: int) -> void:
	peer.create_server(port, 4)
	multiplayer.multiplayer_peer = peer
	multiplayer.peer_connected.connect(_on_peer_connected)
	multiplayer.peer_disconnected.connect(_on_peer_disconnected)

func join(address: String, port: int) -> void:
	peer.create_client(address, port)
	multiplayer.multiplayer_peer = peer
	multiplayer.connected_to_server.connect(_on_connected)
	multiplayer.connection_failed.connect(_on_failed)
	multiplayer.server_disconnected.connect(_on_server_gone)

func _on_peer_connected(id: int) -> void:
	players.append(id)

func _on_peer_disconnected(id: int) -> void:
	players.erase(id)

func _on_connected() -> void:
	players.append(multiplayer.get_unique_id())

func _on_failed() -> void:
	players.clear()

func _on_server_gone() -> void:
	players.clear()
	multiplayer.multiplayer_peer = null

@rpc("any_peer", "reliable")
func chat(text: String) -> void:
	print(multiplayer.get_remote_sender_id(), ": ", text)
