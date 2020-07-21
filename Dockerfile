FROM golang:1.14.4

RUN apt-get update
RUN apt-get install -y libjansson-dev
RUN apt-get install -y libreadline-dev
RUN apt-get install -y graphviz

RUN mkdir -p /go/src/github.com/turbio/evaldb

WORKDIR /go/src/github.com/turbio/evaldb

COPY . .

RUN make

RUN mkdir /db

EXPOSE 5000

ENTRYPOINT ["./gateway", "-path", "/db"]
