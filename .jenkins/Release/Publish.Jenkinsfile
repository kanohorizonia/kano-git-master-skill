@Library(['kano-jenkins-common-pipeline-library@main', 'kano-jenkins-agent-skill-pipeline-library@main']) _

pipeline {
    agent none

    parameters {
        booleanParam(name: 'DRY_RUN', defaultValue: true, description: 'Render Release/Publish plan only by default; no site publish or GitHub Release is created.')
        booleanParam(name: 'PUBLISH_RELEASE', defaultValue: false, description: 'Run Release/Publish. Non-dry-run publishing requires explicit approval and release tag.')
        string(name: 'RELEASE_TAG', defaultValue: '', description: 'Required for non-dry-run GitHub Release publishing, for example v0.1.1.')
    }

    stages {
        stage('Run Agent Skill Release/Publish') {
            steps {
                script {
                    Map parameterProfile = agentSkillParameterProfile(
                        params: params,
                        githubRepository: 'kanohorizonia/kano-git-master-skill',
                        githubTokenCredentialId: 'github-release-token',
                        siteBuildCommand: './scripts/kog site build',
                        sitePublishCommand: './scripts/kog site publish',
                        releaseApprovalMessage: 'Approve kano-git-master-skill Release/Publish? Confirm tag, artifacts, site output, and GitHub release plan before proceeding.',
                    )

                    agentSkillPipeline(parameterProfile + [
                        projectName: 'kano-git-master-skill',
                        bootstrapAgentLabel: 'lightweight',
                        windowsAgentLabel: 'windows && lightweight',
                        linuxAgentLabel: 'linux && lightweight',
                        macAgentLabel: 'mac && lightweight',

                        bDryRun: params.DRY_RUN,
                        bRunBuildPhase: false,
                        bRunReleasePhase: params.PUBLISH_RELEASE,
                        bPublishSite: params.PUBLISH_RELEASE,
                        bCreateGitHubRelease: params.PUBLISH_RELEASE,
                        bRequireExplicitReleaseTag: true,
                        bRequireReleaseApproval: true,
                        bStageSiteInputs: true,
                        bArchiveSiteArtifacts: true,

                        releaseTag: params.RELEASE_TAG,
                        releaseTitle: params.RELEASE_TAG,
                        releaseDraft: true,
                        releasePrerelease: false,
                    ])
                }
            }
        }
    }
}
