cmd_/workspace/drivers/kcr/Module.symvers :=  sed 's/ko$$/o/'  /workspace/drivers/kcr/modules.order | scripts/mod/modpost -m      -o /workspace/drivers/kcr/Module.symvers -e -i Module.symvers -T - 
