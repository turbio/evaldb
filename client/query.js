'use strict';

const e = React.createElement;

const funcSyntax = {
  luaval: {
    headOpen: `function(`,
    headClose: `)`,
    tail: 'end',
    placeholder: '-- your code here',
  },
  duktape: {
    headOpen: `function(`,
    headClose: `) {`,
    tail: '}',
    placeholder: '// your code here',
  },
}[lang];

class Root extends React.Component {
  constructor(props) {
    super(props);
    this.state = {
      gens: {
        '0': { type: 'initial', children: [], id: 0 },
      },
      head: 0,
      newQuery: {
        args: [],
        code: '',
      },
    };
  }

  setQuery(newQuery) {
    this.setState({ newQuery: { ...this.state.newQuery, ...newQuery } });
  }

  mergeInTransaction(t) {
    const { gens, head } = this.state;

    const existing = gens[t.result.gen];

    if (existing) {
      t.children = existing.children;
    } else {
      t.children = [];
    }

    t.type = 'gen';
    t.id = t.result.gen;

    // fake a parent for a little while
    if (!gens[t.result.parent]) {
      gens[t.result.parent] = {
        result: { gen: t.result.parent },
        query: {},
        children: [],
        type: 'gen',
        id: t.result.parent,
      };
    }

    const parent = gens[t.result.parent];

    if (parent.children.filter(c => c.id === t.id).length === 0) {
      parent.children.push(t);
      parent.children.sort((l, r) => l.id - r.id);
    }

    this.setState({
      gens: {
        ...gens,
        [t.result.gen]: t,
      },
      head: head < t.id && !t.result.error ? t.id : head,
    });
  }

  doQuery() {
    const {
      head,
      newQuery: { code, args: aargs },
    } = this.state;

    const args = aargs.reduce((sum, c) => {
      if (!validJSON(c.value)) {
        sum[c.name] = 'invalid json!';
        return sum;
      }

      sum[c.name] = JSON.parse(c.value);

      return sum;
    }, {});

    fetch('/eval/' + dbname, {
      method: 'POST',
      headers: {
        'Content-Type': 'application/json',
      },
      body: JSON.stringify({
        code,
        args,
        // gen: head,
      }),
    })
      .then(res => res.json())
      .then(result => {
        this.mergeInTransaction({ query: { args, code }, result });
      });

    this.setQuery({ code: '', args: [] });
  }

  componentWillMount() {
    const es = new EventSource('/tail/' + dbname);
    es.addEventListener('transac', e => {
      const t = JSON.parse(e.data);
      this.mergeInTransaction(t);
    });
  }

  render() {
    const { gens, head, newQuery } = this.state;

    return e(Entry, {
      gen: gens[0],
      head,
      newQuery,
      setQuery: this.setQuery.bind(this),
      doQuery: this.doQuery.bind(this),
    });
  }
}

const Timeline = props =>
  e('div', { className: 'timeline fork' }, e(Entry, props));

const Entry = ({
  gen: { result, query, type, children, id },
  head,
  newQuery,
  setQuery,
  doQuery,
}) => [
  type === 'initial'
    ? e(InitialGen, { key: id })
    : e(Gen, { result, query, key: id }),
  ...children.map((ch, i, arr) =>
    e(i !== arr.length - 1 || id === head ? Timeline : Entry, {
      gen: ch,
      head,
      key: ch.id,
      newQuery,
      setQuery,
      doQuery,
    }),
  ),
  id === head
    ? e(Input, { key: 'input-' + head, newQuery, setQuery, doQuery })
    : undefined,
];

class Input extends React.Component {
  componentDidMount() {
    this.textarea && this.textarea.focus();
    this.entry && this.entry.scrollIntoView();
  }

  render() {
    return e(
      'div',
      {
        className: 'entry athead',
        ref: entry => (this.entry = entry),
      },
      e(
        'div',
        { className: 'query' },
        e(
          'div',
          {
            className: 'query-box',
          },
          e(EditArgs, {
            args: this.props.newQuery.args,
            setQuery: this.props.setQuery,
          }),
          e('textarea', {
            className: 'query-code',
            rows: 1,
            key: 'query',
            placeholder: funcSyntax.placeholder,
            ref: textarea => {
              this.textarea = textarea;
            },
            value: this.props.newQuery.code,
            onChange: e => {
              if (!this.textarea) return;

              const lines = this.textarea.value.split('\n').length;

              if (this.textarea.attributes['rows'].value < lines) {
                this.textarea.attributes['rows'].value = lines;
              }

              this.props.setQuery({
                code: this.textarea.value,
              });
            },
            onKeyPress: e => {
              if (e.key === 'Enter' && e.ctrlKey) {
                this.props.doQuery();
              }
            },
          }),
          e('div', { className: 'func-head' }, funcSyntax.tail),
        ),
        e(
          'button',
          {
            className: 'go-query',
            onClick: () => {
              this.props.doQuery();
            },
          },
          '\xA0go\xA0',
        ),
      ),
    );
  }
}

const validJSON = str => {
  try {
    JSON.parse(str);
  } catch (e) {
    return false;
  }

  return true;
};

const validArg = str => !!str.match(/^[a-zA-Z][a-zA-Z0-9]*$/);

const Args = ({ args }) =>
  e(
    'div',
    { className: 'func-head' },
    funcSyntax.headOpen,
    Object.keys(args)
      .map(k => k + ' = ' + JSON.stringify(args[k]))
      .join(', '),
    funcSyntax.headClose,
  );

class ArgInput extends React.Component {
  componentDidMount() {
    this.nameInput && this.nameInput.focus();
  }

  render() {
    const { a, i, arr, setQuery } = this.props;

    return e(
      'span',
      { key: i },
      e('input', {
        type: 'text',
        key: 'key-' + i,
        placeholder: 'name',
        ref: r => (this.nameInput = r),
        className: 'arg' + (!validArg(a.name) ? ' err' : ''),
        size: a.name.length > 4 ? a.name.length : 4,
        value: a.name,
        onChange: e => {
          if (e.target.value.slice(-1) === '=') {
            document.getElementById('value-' + i).focus();
            return;
          }

          setQuery({
            args: [
              ...arr.slice(0, i),
              { ...a, name: e.target.value },
              ...arr.slice(i + 1),
            ],
          });
        },
      }),
      ' = ',
      e('input', {
        type: 'text',
        key: 'value-' + i,
        id: 'value-' + i,
        placeholder: 'json',
        className: 'arg' + (!validJSON(a.value) ? ' err' : ''),
        size: a.value.length > 4 ? a.value.length : 4,
        value: a.value,
        onChange: e =>
          e.target.value.slice(-1) === ','
            ? setQuery({ args: [...arr, { name: '', value: '' }] })
            : setQuery({
                args: [
                  ...arr.slice(0, i),
                  { ...a, value: e.target.value },
                  ...arr.slice(i + 1),
                ],
              }),
      }),
      i !== arr.length - 1 ? ', ' : undefined,
    );
  }
}

const EditArgs = ({ args, setQuery }) =>
  e(
    'div',
    { className: 'func-head' },
    funcSyntax.headOpen,
    args.map((a, i, arr) => e(ArgInput, { key: i, a, i, arr, setQuery })),
    e(
      'button',
      {
        className: 'arg',
        onClick: () => setQuery({ args: [...args, { name: '', value: '' }] }),
      },
      '+',
    ),
    funcSyntax.headClose,
  );

const InitialGen = () =>
  e('div', { className: 'entry initial-msg' }, 'initial state');

const Gen = ({
  result: { gen, parent, walltime, warn, error, object, warm },
  query: { code, args },
}) =>
  e(
    'div',
    { className: 'entry' + (error ? ' error' : '') },
    e(
      'div',
      { className: 'query readonly' },
      e(
        'div',
        { className: 'query-box' },
        e(Args, { args }),
        e('textarea', {
          className: 'query-code',
          readOnly: true,
          value: code,
          rows: Math.min(10, code.split('\n').length),
        }),
        e('div', { className: 'func-head' }, funcSyntax.tail),
      ),
    ),
    e(
      'div',
      { className: 'status-line' },
      'gen: ' +
        gen +
        ' | ' +
        'pgen: ' +
        parent +
        ' | ' +
        'walltime: ' +
        walltime / 1e6 +
        'ms' +
        ' | ' +
        (warm ? 'warm' : 'cold'),
      e(asCode, { code, args }),
    ),
    error
      ? e('div', { className: 'result error' }, JSON.stringify(error, null, 2))
      : e('div', { className: 'result' }, JSON.stringify(object, null, 2)),
  );

class asCode extends React.Component {
  constructor(props) {
    super(props);
    this.state = { selected: null };
  }

  render() {
    const { code, args } = this.props;
    const { selected } = this.state;

    const url =
      window.location.protocol +
      '//' +
      window.location.host +
      '/eval/' +
      dbname;

    const data = JSON.stringify({
      code,
      args: Object.keys(args).length ? args : undefined,
    });

    const snippets = {
      curl: `curl '${url}' -H 'Content-Type: application/json' --data '${data}'`,
      fetch: `fetch("${url}", {
  method: "POST",
  headers: { "Content-Type": "application/json" },
  body: JSON.stringify(${data}),
})
.then(res => res.json())
.then(res => console.log(res));`,
    };

    let elem = null;

    if (selected) {
      elem = e(
        'div',
        {
          className: 'as-code-box',
          key: 'box',
        },
        e(
          'pre',
          {
            style: {
              resize: 'none',
              border: 'none',
              width: '100%',
              whiteSpace: 'pre-wrap',
              cursor: 'text',
            },
            onClick: e => {
              const range = document.createRange();
              range.selectNode(e.target);
              getSelection().removeAllRanges();
              getSelection().addRange(range);
            },
          },
          snippets[selected],
        ),
      );
    }

    const sel = s => () =>
      this.setState({ selected: this.state.selected === s ? null : s });

    return [
      e(
        'span',
        { style: { float: 'right' }, key: 'sp' },
        'as: ',
        e(
          'button',
          { className: 'as-code-button', onClick: sel('curl') },
          'cURL',
        ),
        e(
          'button',
          { className: 'as-code-button', onClick: sel('fetch') },
          'fetch',
        ),
      ),
      elem,
    ];
  }
}

const dom = document.querySelector('#timeline');
ReactDOM.render(e(Root), dom);
