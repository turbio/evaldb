var routes = {
  GET: {},
  POST: {},
};

var register = function(method, path, router) {
  routes[method][path] = router;
};

var req = function(r) {
  if (!routes[r.method]) {
    return 'unknown method "' + r.method + '"';
  }

  if (routes[r.method][r.path]) {
    return routes[r.method][r.path](r);
  }

  return 'cannot ' + r.method + ' ' + r.path;
};

this.http = {
  get: function(path, router) {
    register('GET', path, router);
  },
  post: function(path, router) {
    register('POST', path, router);
  },
  req: req,
};
