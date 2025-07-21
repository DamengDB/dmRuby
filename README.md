# DmRuby

该仓库主要提供Ruby访问达梦数据库的编程接口。

## 主要功能

支持的功能包括连接数据库，执行语句，绑定参数执行语句以及获取结果集等功能。

## 使用方法

1、在源码目录下执行以下命令生成gem

```
gem build dm.gemspec
```

2、ruby-dm安装：
方式一：配置DM_HOME,DM_HOME是达梦数据库的安装目录，配置完成后使用命令 

```
gem install ruby-dm-x.x.x.gem
```

方式二：直接指定dpi的目录

```
gem install ruby-dm-x.x.x.gem -- --with-dm-include=E:\dmdbms\include --with-dm-lib=E:\dmdbms\drivers\dpi
```

