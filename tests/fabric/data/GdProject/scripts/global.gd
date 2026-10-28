extends Node
## The project's autoload: shared score.

var score := 0

func add_points(n: int) -> void:
	score += n
