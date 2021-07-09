def fib(i):
	if i == 1 or i == 2:
		return 1
	else:
		return fib(i-1) + fib(i-2)

print(fib(10))
