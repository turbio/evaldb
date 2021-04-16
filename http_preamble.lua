local routes = {
	GET  = {},
	POST = {},
}

local register = function(method, path, router)
	assert(type(method) == 'string', 'the method must be a string')
	assert(routes[method], 'the method must be known')
	assert(type(path) == 'string', 'the path must be a string')
	assert(type(router) == 'function', 'the router callback must be a function')

	routes[method][path] = router
end

local req = function(r)
	if not routes[r.method] then
		return 'unknown method "' .. r.method .. '"'
	end

	if routes[r.method][r.path] then
		return routes[r.method][r.path](r)
	end

	return 'cannot ' .. r.method .. ' ' .. r.path
end

http = {
	get  = function(path, router) register('GET', path, router) end,
	post = function(path, router) register('POST', path, router) end,
	req = req,
}
