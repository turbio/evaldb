const fetch = require('node-fetch');

function connect(key) {
  async function query(code, args, readonly = true) {
    if (typeof code !== 'string') {
      throw new Error('expected query code to be a string');
    }
    if (!args && typeof args !== 'undefined') {
      throw new Error('expected query args to be an object or undefined');
    }
    if (args && (typeof args !== 'object' || Array.isArray(args))) {
      throw new Error('expected query args to be an object or undefined');
    }
    if (typeof readonly !== 'boolean') {
      throw new Error('expected query readonly to be a boolean');
    }

    const req = await fetch('https://evaldb.turb.io/eval/' + key, {
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ code, args, readonly }),
      method: 'POST',
    });

    const resj = await req.json();

    if (resj.error) {
      throw resj.error;
    }

    return resj.object;
  }

  return {
    read(code, args) {
      return query(code, args);
    },
    write(code, args) {
      return query(code, args, false);
    },
    query,
  };
}

module.exports = connect;
