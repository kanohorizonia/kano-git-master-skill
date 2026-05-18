@Library(['kano-jenkins-common-pipeline-library@main', 'kano-jenkins-agent-skill-pipeline-library@main']) _

pipeline {
    agent none

    // First-run dogfood jobs stay safe by default: DRY_RUN=true renders the plan
    // before checkout/build/export side effects. Task 4 found strict release
    // archive validation can fail after producing an archive, so non-dry-run
    // export validation should be reviewed before enabling unattended runs.

    parameters {
        booleanParam(name: 'DRY_RUN', defaultValue: true, description: 'Render the effective command plan and stop before checkout/build/release.')
        booleanParam(name: 'PUBLISH_RELEASE', defaultValue: false, description: 'Run the internal Release/Publish review phase only; external publish actions remain disabled.')
        string(name: 'RELEASE_TAG', defaultValue: '', description: 'Optional review tag label; not required because external release publishing is disabled.')
    }

    stages {
        stage('Run Agent Skill Pipeline') {
            steps {
                script {
                    Map parameterProfile = agentSkillParameterProfile(params: params)

                    agentSkillPipeline(parameterProfile + [
                        projectName: 'kano-git-master-skill',
                        bootstrapAgentLabel: 'lightweight',
                        windowsAgentLabel: 'windows && lightweight',
                        linuxAgentLabel: 'linux && lightweight',
                        macAgentLabel: 'mac && lightweight',

                        bDryRun: params.DRY_RUN,
                        bRunReleasePhase: params.PUBLISH_RELEASE,
                        bPublishSite: false,
                        bCreateGitHubRelease: false,
                        bRequireExplicitReleaseTag: false,
                        bStageSiteInputs: false,
                        bArchiveSiteArtifacts: false,
                        releaseTag: params.RELEASE_TAG,
                        releaseTitle: params.RELEASE_TAG,

                        windowsBuildCommand: 'scripts\\kog.bat self build',
                        windowsValidateCommand: 'bash -lc "src/shell/test/pre-commit-quality-gate.sh"',
                        windowsExportCommand: 'bash -lc "./scripts/kog export --single --validate-release-archive"',
                        linuxBuildCommand: './scripts/kog self build',
                        linuxValidateCommand: 'src/shell/test/pre-commit-quality-gate.sh',
                        linuxExportCommand: './scripts/kog export --single --validate-release-archive',
                        macBuildCommand: './scripts/kog self build',
                        macValidateCommand: 'src/shell/test/pre-commit-quality-gate.sh',
                        macExportCommand: './scripts/kog export --single --validate-release-archive',
                    ])
                }
            }
        }
    }
}
