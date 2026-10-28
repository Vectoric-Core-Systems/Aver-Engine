class_name Calc
extends RefCounted
## Pure functions the F4b tests run after conversion: control flow, match, ranges, string formatting, Variant arithmetic.

enum Kind { SMALL, MEDIUM, LARGE }

static func classify(n: int) -> String:
	match n:
		0:
			return "zero"
		1, 2, 3:
			return "few"
		_:
			if n < 0:
				return "negative"
			return "many"

static func fizzbuzz(limit: int) -> String:
	var parts: Array[String] = []
	for i in range(1, limit + 1):
		if i % 15 == 0:
			parts.append("FizzBuzz")
		elif i % 3 == 0:
			parts.append("Fizz")
		elif i % 5 == 0:
			parts.append("Buzz")
		else:
			parts.append(str(i))
	return ",".join(parts)

static func format_stats(name: String, hp: int, ratio: float) -> String:
	return "%s has %d hp (%.1f%%) [%05d|%-4s|%+d|%x]" % [name, hp, ratio * 100.0, hp, "ab", hp, 255]

static func int_math(a: int, b: int) -> Array:
	return [a / b, a % b, -7 / 2, -7 % 3, 2 ** 10, a * b]

static func numbers() -> Array:
	return [posmod(-7, 3), wrapi(7, 0, 5), snapped(7.3, 0.5), roundi(2.5), roundi(-2.5), floori(-2.5), ceili(2.1), absi(-4), maxi(3, 9),
			clampi(15, 0, 10), sign(-3.2), int("12abc"), "7".to_int() + 1, 10 / 4, 10 / 4.0, 7 % 3, 2 ** 0.5 > 1.4]

static func collatz(n: int) -> int:
	var steps := 0
	while n != 1:
		n = n / 2 if n % 2 == 0 else 3 * n + 1
		steps += 1
	return steps

static func sum_dict(d: Dictionary) -> int:
	var total := 0
	for k in d:
		total += d[k]
	return total

static func word_count(text: String) -> Dictionary:
	var counts := {}
	for w in text.split(" "):
		if w.is_empty():
			continue
		counts[w] = counts.get(w, 0) + 1
	return counts

static func evens_squared(items: Array[int]) -> Array[int]:
	var out: Array[int] = []
	for i in items:
		if i % 2 == 0:
			out.append(i * i)
	return out

static func lerp_steps(a: float, b: float, n: int) -> Array[float]:
	var out: Array[float] = []
	for i in range(n + 1):
		out.append(lerp(a, b, float(i) / n))
	return out

static func kind_of(x: int) -> Kind:
	if x < 10:
		return Kind.SMALL
	elif x < 100:
		return Kind.MEDIUM
	return Kind.LARGE

static func is_palindrome(s: String) -> bool:
	var cleaned := ""
	for c in s.to_lower():
		if c >= "a" and c <= "z":
			cleaned += c
	var n := cleaned.length()
	for i in range(n / 2):
		if cleaned[i] != cleaned[n - 1 - i]:
			return false
	return true

static func shape_area(shape: Array) -> float:
	match shape:
		["circle", var r]:
			return PI * r * r
		["rect", var w, var h]:
			return w * h
		["square", var s] when s > 0:
			return s * s
		_:
			return -1.0

static func inventory_report(items: Dictionary) -> String:
	var lines: Array[String] = []
	for key in items:
		var qty: int = items[key]
		lines.append("%s x%d" % [key, qty] if qty > 1 else str(key))
	return ", ".join(lines)

static func transform(items: Array[int]) -> Array:
	var doubled := items.map(func(i): return i * 2)
	var big := items.filter(func(i): return i > 2)
	var total := items.reduce(func(acc, i): return acc + i, 0)
	return [doubled, big, total]

static func sorted_desc(items: Array[int]) -> Array[int]:
	var copy := items.duplicate()
	copy.sort_custom(func(a, b): return a > b)
	return copy

static func untyped_sum(values: Array) -> Variant:
	var total = 0
	for v in values:
		total += v
	return total

static func countdown_sum(from: int) -> int:
	var sum := 0
	for i in range(from, 0, -1):
		sum += i
	return sum

static func contains_all(haystack: Array, needles: Array) -> bool:
	for n in needles:
		if not n in haystack:
			return false
	return true

static func truthy_count(values: Array) -> int:
	var count := 0
	for v in values:
		if v:
			count += 1
	return count
