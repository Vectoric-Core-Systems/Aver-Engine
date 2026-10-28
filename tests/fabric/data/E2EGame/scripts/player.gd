class_name Player
extends CharacterBody3D

@export var speed := 5.0

signal hit(damage)

func _physics_process(delta):
	var axis := Input.get_axis("jump", "interact")
	velocity.x = axis * speed
	velocity.y -= 9.8 * delta
	move_and_slide()

func take_damage(amount: int) -> void:
	hit.emit(amount)
