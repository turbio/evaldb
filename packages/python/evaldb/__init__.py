import requests


class QueryException(Exception):
    pass


class DB():
    def __init__(self, key=None):
        if type(key) != str:
            raise TypeError("EvalDB key must be a string")

        self.key = key

    def read(self, query, **args):
        return self.query(query, args, True)

    def write(self, query, **args):
        return self.query(query, args, False)

    def query(self, query, args, readonly=False):
        httpres = requests.post(
            'https://evaldb.turb.io/eval/' + self.key,
            json={
                'code': query,
                'readonly': readonly,
                'args': args,
            },
        )

        dbres = httpres.json()

        if 'error' in dbres:
            raise QueryException(dbres['error'])

        return dbres['object']
